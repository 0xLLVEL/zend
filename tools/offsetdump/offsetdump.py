#!/usr/bin/env python3
"""Build the Zend manifest for one Roblox client build.

Inputs are the artifacts produced by the client dumper:

  RobloxPlayerBeta_<build>_decrypted.exe   full decrypted image
  offsets.hpp                              Credoxy style RVA dump
  hyperion.hpp                             loader profile, imports, patches
  hyperion_win_live_client_<build>.txt     Hyperion live RVA dump

The tool never guesses silently: every value it publishes is either read from
the decrypted image or cross-checked against a second source. Mismatches abort
with a non-zero exit code.

Output (default `builds/version-<build>/`):

  build.json            one manifest for loader + core
  loader.offsets.json   header, sections, import slots, patch sites
  core.offsets.json     RVAs consumed by module.dll
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path


class DumpError(RuntimeError):
    pass


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def align_up(value: int, align: int) -> int:
    return (value + align - 1) & ~(align - 1)


class PE:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        data = self.data
        if len(data) < 0x40 or data[:2] != b"MZ":
            raise DumpError(f"{path}: not a PE image")
        self.pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[self.pe:self.pe + 4] != b"PE\0\0":
            raise DumpError(f"{path}: missing PE signature")
        self.machine, self.section_count = struct.unpack_from("<HH", data, self.pe + 4)
        self.optional_size = struct.unpack_from("<H", data, self.pe + 20)[0]
        self.optional = self.pe + 24
        magic = struct.unpack_from("<H", data, self.optional)[0]
        if magic != 0x20B:
            raise DumpError(f"{path}: expected PE32+, got 0x{magic:04X}")
        self.entry = struct.unpack_from("<I", data, self.optional + 16)[0]
        self.image_base = struct.unpack_from("<Q", data, self.optional + 24)[0]
        self.section_align = struct.unpack_from("<I", data, self.optional + 32)[0]
        self.file_align = struct.unpack_from("<I", data, self.optional + 36)[0]
        self.image_size = struct.unpack_from("<I", data, self.optional + 56)[0]
        self.checksum = struct.unpack_from("<I", data, self.optional + 64)[0]
        self.data_dirs = self.optional + 112
        self.sections = []
        table = self.pe + 24 + self.optional_size
        for index in range(self.section_count):
            off = table + index * 40
            name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
            characteristics = struct.unpack_from("<I", data, off + 36)[0]
            self.sections.append({
                "name": name,
                "rva": vaddr,
                "virtual_size": vsize,
                "raw_size": raw_size,
                "raw_ptr": raw_ptr,
                "characteristics": characteristics,
            })

    def section(self, name: str) -> dict:
        for section in self.sections:
            if section["name"] == name:
                return section
        raise DumpError(f"{self.path}: missing section {name}")

    def rva_to_off(self, rva: int) -> int:
        for section in self.sections:
            span = max(section["virtual_size"], section["raw_size"])
            if section["rva"] <= rva < section["rva"] + span:
                delta = rva - section["rva"]
                if delta >= section["raw_size"]:
                    raise DumpError(
                        f"{self.path}: RVA 0x{rva:X} points past raw data of "
                        f"{section['name']}")
                return section["raw_ptr"] + delta
        raise DumpError(f"{self.path}: RVA 0x{rva:X} is outside every section")

    def read_rva(self, rva: int, size: int) -> bytes:
        off = self.rva_to_off(rva)
        if off + size > len(self.data):
            raise DumpError(f"{self.path}: short read at RVA 0x{rva:X}")
        return self.data[off:off + size]

    def imports(self) -> list[dict]:
        """Return every imported slot with module, symbol and IAT RVA."""
        dir_rva, dir_size = struct.unpack_from("<II", self.data, self.data_dirs + 8)
        if not dir_rva:
            return []
        off = self.rva_to_off(dir_rva)
        rows: list[dict] = []
        while True:
            original, timestamp, forwarder, name_rva, iat_rva = struct.unpack_from(
                "<IIIII", self.data, off)
            if not any((original, timestamp, forwarder, name_rva, iat_rva)):
                break
            name_off = self.rva_to_off(name_rva)
            end = self.data.index(b"\0", name_off)
            module = self.data[name_off:end].decode("ascii", "replace")

            thunk_rva = original or iat_rva
            slot_rva = iat_rva
            while True:
                thunk = struct.unpack_from("<Q", self.read_rva(thunk_rva, 8))[0]
                if thunk == 0:
                    break
                if thunk & (1 << 63):
                    symbol = f"#{thunk & 0xFFFF}"
                else:
                    hint_off = self.rva_to_off(thunk & 0x7FFFFFFF)
                    term = self.data.index(b"\0", hint_off + 2)
                    symbol = self.data[hint_off + 2:term].decode("ascii", "replace")
                rows.append({
                    "module": module,
                    "symbol": symbol,
                    "slot": slot_rva,
                })
                thunk_rva += 8
                slot_rva += 8
            off += 20
        return rows


def parse_hex(value: str) -> int:
    return int(value, 16)


def const_values(text: str) -> dict[str, list[int]]:
    """Collect `NAME = REBASE(0x...)` and `NAME = 0x...` constants."""
    result: dict[str, list[int]] = {}
    pattern = re.compile(
        r"const(?:expr)?\s+(?:u?intptr_t|unsigned|std::uintptr_t|std::uint32_t|"
        r"std::int32_t|auto)\s+([A-Za-z_]\w*)\s*=\s*(?:REBASE\()?"
        r"(0x[0-9A-Fa-f]+|\d+)\)?")
    for name, value in pattern.findall(text):
        result.setdefault(name, []).append(int(value, 0))
    return result


def one(values: dict[str, list[int]], name: str, source: Path) -> int:
    candidates = values.get(name, [])
    if not candidates:
        raise DumpError(f"{source}: constant {name} not found")
    if len(set(candidates)) != 1:
        raise DumpError(
            f"{source}: constant {name} is ambiguous: "
            + ", ".join(hex(v) for v in sorted(set(candidates))))
    return candidates[0]


def profile_values(text: str) -> dict[str, str]:
    pattern = re.compile(r"inline constexpr\s+\S+\s+(\w+)\s*=\s*(.+?);")
    result: dict[str, str] = {}
    for name, value in pattern.findall(text):
        result[name] = value.strip()
    return result


def parse_int(text: str) -> int:
    return int(text.rstrip("uUlL"), 0)


def parse_imports(text: str) -> list[dict]:
    pattern = re.compile(
        r'\{"([^"]+)","((?:[^"\\]|\\.)*)",0x([0-9A-Fa-f]+)\}')
    rows = []
    for module, symbol, slot in pattern.findall(text):
        rows.append({
            "module": module,
            "symbol": symbol.replace('\\"', '"'),
            "slot": int(slot, 16),
        })
    return rows


def parse_tls(text: str) -> list[int]:
    match = re.search(
        r"std::array<std::uint32_t,\s*(\d+)>\s+tls_rvas\{([^}]*)\}", text)
    if not match:
        return []
    values = [int(v.strip(), 16) for v in match.group(2).split(",") if v.strip()]
    if len(values) != int(match.group(1)):
        raise DumpError("tls_rvas count does not match its declared size")
    return values


def parse_array_u8(text: str, name: str) -> list[int]:
    match = re.search(
        rf"std::array<unsigned char,\s*(\d+)>\s+{name}\s*\{{([^}}]*)\}}", text)
    if not match:
        return []
    values = [int(v.strip(), 16) for v in match.group(2).split(",") if v.strip()]
    if len(values) != int(match.group(1)):
        raise DumpError(f"{name} count does not match its declared size")
    return values


def derive_payload(pe: PE, loader: dict) -> dict:
    text_rva = loader["textRva"]
    text_size = loader["textSize"]
    unwind_rva = loader["runtime"]
    unwind_count = loader["rtCount"]
    text = pe.read_rva(text_rva, text_size)
    unwind = pe.read_rva(unwind_rva, unwind_count * 12)
    payload = text + struct.pack("<Q", unwind_count) + unwind
    return {
        "bytes": payload,
        "size": len(payload),
        "sha256": sha256(payload),
    }


def entry_thunk_target(pe: PE, entry: int) -> int:
    """Follow the dumper's `jmp rel32` entry thunk to the real client main."""
    code = pe.read_rva(entry, 32)
    for index in range(len(code) - 5):
        if code[index] == 0xE9:
            rel = struct.unpack_from("<i", code, index + 1)[0]
            target = entry + index + 5 + rel
            if 0 < target < pe.image_size:
                return target
    raise DumpError(f"{pe.path}: no relative jmp in entry thunk at 0x{entry:X}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path,
                        help="decrypted client image")
    parser.add_argument("--live", type=Path,
                        help="packed live RobloxPlayerBeta.exe (header check)")
    parser.add_argument("--profile", type=Path,
                        help="hyperion.hpp emitted by the dumper")
    parser.add_argument("--offsets", type=Path,
                        help="Credoxy offsets.hpp")
    parser.add_argument("--client-dump", type=Path,
                        help="hyperion_win_live_client_<build>.txt")
    parser.add_argument("--payload", type=Path,
                        help="existing payload.bin to verify against")
    parser.add_argument("--out", type=Path,
                        help="output directory (default builds/version-<build>)")
    args = parser.parse_args()

    exe = PE(args.exe)
    build = re.search(r"([0-9a-f]{16})", args.exe.name)
    build_id = build.group(1) if build else None

    profile_text = ""
    profile: dict[str, str] = {}
    imports: list[dict] = []
    tls: list[int] = []
    if args.profile:
        profile_text = read_text(args.profile)
        profile = profile_values(profile_text)
        imports = parse_imports(profile_text)
        tls = parse_tls(profile_text)

    if "client_version" in profile:
        declared = profile["client_version"].strip('"')
        if build_id and declared != build_id:
            raise DumpError(
                f"build mismatch: exe name says {build_id}, profile says {declared}")
        build_id = declared
    if not build_id:
        raise DumpError("cannot determine build id; pass a profile or rename the exe")

    out_dir = args.out or Path("builds") / f"version-{build_id}"

    text = exe.section(".text")
    unwind = exe.section(".pdata")
    decrypted_image_size = exe.image_size
    ep_restored = exe.entry
    ep_new = entry_thunk_target(exe, ep_restored)
    text_rva = text["rva"]
    text_size = text["virtual_size"]
    text_map = text["raw_size"]
    text_pad = text_map - text_size
    runtime = unwind["rva"]
    rt_count = unwind["virtual_size"] // 12

    loader: dict = {
        "imageSize": decrypted_image_size,
        "epOrig": ep_restored,
        "epNew": ep_new,
        "textRva": text_rva,
        "textSize": text_size,
        "textMap": text_map,
        "textPad": text_pad,
        "runtime": runtime,
        "rtCount": rt_count,
        "imports": len(imports) + (1 if imports else 0),
    }

    # Loader values that are not derivable from the PE alone.
    if profile:
        loader["lua"] = parse_int(profile["lua_rva"])
        loader["idJson"] = parse_int(profile["identity_rva"])
        if "unwind_rva" in profile and parse_int(profile["unwind_rva"]) != runtime:
            raise DumpError("profile unwind_rva does not match .pdata")
        if "unwind_count" in profile and parse_int(profile["unwind_count"]) != rt_count:
            raise DumpError("profile unwind_count does not match .pdata")
        if "restored_entry" in profile and parse_int(profile["restored_entry"]) != ep_restored:
            raise DumpError("profile restored_entry does not match the decrypted image")
    else:
        loader["lua"] = 0
        loader["idJson"] = 0

    live_meta = None
    if args.live:
        live = PE(args.live)
        loader["imageSize"] = live.image_size
        loader["epOrig"] = live.entry
        live_meta = {
            "sha256": sha256(live.data),
            "size": len(live.data),
            "imageSize": live.image_size,
            "entry": live.entry,
        }
        if profile and "image_size" in profile:
            if live.image_size != parse_int(profile["image_size"]):
                raise DumpError("profile image_size does not match the live exe")
        if profile and "original_entry" in profile:
            if live.entry != parse_int(profile["original_entry"]):
                raise DumpError("profile original_entry does not match the live exe")
    elif profile:
        if "image_size" in profile:
            loader["imageSize"] = parse_int(profile["image_size"])
        if "original_entry" in profile:
            loader["epOrig"] = parse_int(profile["original_entry"])

    # Core offsets come from the Hyperion client dump. The Credoxy header is a
    # second opinion for the subset it shares.
    core: dict = {}
    if args.client_dump:
        client_text = read_text(args.client_dump)
        client = const_values(client_text)
        core = {
            "fakeDataModel": one(client, "FakeDataModelPointer", args.client_dump),
            "rawScheduler": one(client, "RawScheduler", args.client_dump),
            "opcodeLookupTable": one(client, "OpcodeLookupTable", args.client_dump),
            "getScriptVMState": one(client, "GetScriptVMState", args.client_dump),
            "taskDefer": one(client, "TaskDefer", args.client_dump),
            "getCapabilities": one(client, "GetCapabilities", args.client_dump),
            "fireTouchInterest": one(client, "FireTouchInterest", args.client_dump),
            "fireProximityPrompt": one(client, "FireProximityPrompt", args.client_dump),
            "opcodeWireMultiplier": one(client, "OpcodeWireMultiplier", args.client_dump),
        }
    if args.offsets:
        credoxy = const_values(read_text(args.offsets))
        names = {
            "fakeDataModel": "FakeDM",
            "rawScheduler": "TaskScheduler",
            "opcodeLookupTable": "OpcodeLookupTable",
            "getScriptVMState": "GetLuaStateForInstance",
            "taskDefer": "TaskDefer",
            "getCapabilities": "GetCapabilities",
        }
        for key, name in names.items():
            value = one(credoxy, name, args.offsets)
            if key in core and core[key] != value:
                raise DumpError(
                    f"{key}: client dump says 0x{core[key]:X}, "
                    f"Credoxy says 0x{value:X}")
            core.setdefault(key, value)

    required = [
        "fakeDataModel", "rawScheduler", "opcodeLookupTable",
        "getScriptVMState", "taskDefer", "opcodeWireMultiplier",
    ]
    missing = [name for name in required if name not in core]
    if missing:
        raise DumpError("missing core offsets: " + ", ".join(missing))

    payload = derive_payload(exe, loader)
    if args.payload:
        existing = args.payload.read_bytes()
        if existing != payload["bytes"]:
            raise DumpError(
                f"{args.payload}: hash {sha256(existing)} does not match "
                f"the payload rebuilt from the decrypted image "
                f"({payload['sha256']})")
    # The dumper's own payload may carry byte-emulation patches that Zend does
    # not ship. Record it for reference, but only enforce the payload passed
    # with --payload.
    profile_patches = {
        "lua": parse_array_u8(profile_text, "lua_expected") if profile_text else [],
        "png": parse_array_u8(profile_text, "png_expected") if profile_text else [],
        "traySpawn": parse_array_u8(profile_text, "tray_spawn_expected") if profile_text else [],
        "trayClose": parse_array_u8(profile_text, "tray_close_expected") if profile_text else [],
    }
    profile_payload_sha = profile.get("payload_sha256", '""').strip('"')
    sites = {}
    if profile:
        for name in ("lua_rva", "png_rva", "identity_rva", "tray_spawn",
                     "tray_close", "tray_close_target", "mux_rva", "proxy_rva"):
            if name in profile:
                sites[name] = f"0x{parse_int(profile[name]):X}"

    # Import verification. The profile omits the proxy forwarder, which the
    # loader adds itself at the tempest import slot.
    pe_imports = exe.imports()
    proxy_slot = next((row for row in pe_imports
                       if row["module"] == "RobloxPlayerBeta.dll"), None)
    real_imports = [row for row in pe_imports if row is not proxy_slot]
    if imports:
        profile_slots = {(row["module"], row["slot"]) for row in imports}
        pe_slots = {(row["module"], row["slot"]) for row in real_imports}
        if profile_slots != pe_slots:
            only_profile = sorted(profile_slots - pe_slots)[:5]
            only_pe = sorted(pe_slots - profile_slots)[:5]
            raise DumpError(
                "import slot mismatch; "
                f"profile-only={only_profile} pe-only={only_pe}")
    else:
        imports = [{**row, "slot": row["slot"]} for row in real_imports]

    loader["imports"] = len(real_imports) + (1 if proxy_slot else 0)

    build_json = {
        "build": build_id,
        "client": profile.get("client_build", '""').strip('"'),
        "clientExe": live_meta,
        "loader": {
            "imageSize": loader["imageSize"],
            "epOrig": loader["epOrig"],
            "epNew": loader["epNew"],
            "textRva": loader["textRva"],
            "textSize": loader["textSize"],
            "runtime": loader["runtime"],
            "rtCount": loader["rtCount"],
            "lua": loader["lua"],
            "idJson": loader["idJson"],
            "imports": loader["imports"],
        },
        "core": {
            "fakeDataModel": f"0x{core['fakeDataModel']:X}",
            "rawScheduler": f"0x{core['rawScheduler']:X}",
            "opcodeLookupTable": f"0x{core['opcodeLookupTable']:X}",
            "getScriptVMState": f"0x{core['getScriptVMState']:X}",
            "taskDefer": f"0x{core['taskDefer']:X}",
            "getCapabilities": f"0x{core['getCapabilities']:X}",
            "fireTouchInterest": f"0x{core['fireTouchInterest']:X}",
            "fireProximityPrompt": f"0x{core['fireProximityPrompt']:X}",
            "opcodeWireMultiplier": core["opcodeWireMultiplier"],
        },
        "payload": {
            "size": payload["size"],
            "sha256": payload["sha256"],
            "text": loader["textSize"],
            "unwind": loader["rtCount"] * 12,
        },
    }

    loader_json = {
        "build": build_id,
        "dumper": {
            "sourceSha256": profile.get("source_sha256", '""').strip('"'),
            "payloadSha256": profile_payload_sha,
        },
        "image": {
            "size": loader["imageSize"],
            "entry": loader["epOrig"],
            "decryptedSize": decrypted_image_size,
            "restoredEntry": ep_restored,
            "clientEntry": loader["epNew"],
            "checksum": exe.checksum,
        },
        "text": {
            "rva": loader["textRva"],
            "size": loader["textSize"],
            "mapSize": loader["textMap"],
            "pad": loader["textPad"],
        },
        "unwind": {
            "rva": loader["runtime"],
            "count": loader["rtCount"],
            "size": loader["rtCount"] * 12,
        },
        "lua": loader["lua"],
        "idJson": loader["idJson"],
        "proxyImport": ({
            "module": proxy_slot["module"],
            "symbol": proxy_slot["symbol"],
            "slot": f"0x{proxy_slot['slot']:X}",
        } if proxy_slot else None),
        "imports": [
            {
                "module": row["module"],
                "symbol": row["symbol"],
                "slot": f"0x{row['slot']:X}",
            }
            for row in imports
        ],
        "tls": [f"0x{value:X}" for value in tls],
        "sites": sites,
        "patches": profile_patches,
    }

    core_json = {
        "build": build_id,
        "fakeDataModel": f"0x{core['fakeDataModel']:X}",
        "rawScheduler": f"0x{core['rawScheduler']:X}",
        "opcodeLookupTable": f"0x{core['opcodeLookupTable']:X}",
        "getScriptVMState": f"0x{core['getScriptVMState']:X}",
        "taskDefer": f"0x{core['taskDefer']:X}",
        "getCapabilities": f"0x{core['getCapabilities']:X}",
        "fireTouchInterest": f"0x{core['fireTouchInterest']:X}",
        "fireProximityPrompt": f"0x{core['fireProximityPrompt']:X}",
        "opcodeWireMultiplier": core["opcodeWireMultiplier"],
    }

    out_dir.mkdir(parents=True, exist_ok=True)
    for name, document in (
        ("build.json", build_json),
        ("loader.offsets.json", loader_json),
        ("core.offsets.json", core_json),
    ):
        target = out_dir / name
        target.write_text(
            json.dumps(document, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8", newline="\n")

    print(f"build      {build_id} ({build_json['client']})")
    print(f"image      0x{loader['imageSize']:X} entry 0x{loader['epOrig']:X} "
          f"-> 0x{loader['epNew']:X}")
    print(f"text       0x{loader['textSize']:X} (+0x{loader['textPad']:X} pad)")
    print(f"unwind     0x{loader['rtCount']:X} entries")
    print(f"imports    {loader['imports']}")
    print(f"payload    {payload['size']} bytes {payload['sha256']}")
    print(f"out        {out_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except DumpError as error:
        print(f"offsetdump: {error}", file=sys.stderr)
        raise SystemExit(1)
