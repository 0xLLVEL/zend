#!/usr/bin/env python3
"""Rebuild the loader payload from a decrypted client image.

Payload layout written by the loader (`restore()`):

    .text VirtualSize bytes
    uint64 little-endian .pdata entry count
    .pdata (count * 12) bytes

`PROFILE.md` calls this the "text + count + unwind" layout. The tool trims
section padding, so the output is independent of how the dumper aligns sections.

Every run asserts the result against `build.json`; a mismatch is a build error,
not a warning. `--archive` additionally packs the payload with the pinned 7-Zip
and checks the archive entry size and CRC.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path


class RepackError(RuntimeError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class PE:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        data = self.data
        if len(data) < 0x40 or data[:2] != b"MZ":
            raise RepackError(f"{path}: not a PE image")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise RepackError(f"{path}: missing PE signature")
        count = struct.unpack_from("<H", data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic != 0x20B:
            raise RepackError(f"{path}: expected PE32+, got 0x{magic:04X}")
        self.sections = []
        table = pe + 24 + opt_size
        for index in range(count):
            off = table + index * 40
            name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
            self.sections.append((name, vaddr, vsize, raw_size, raw_ptr))

    def section(self, name: str) -> tuple:
        for section in self.sections:
            if section[0] == name:
                return section
        raise RepackError(f"{self.path}: missing section {name}")

    def slice(self, rva: int, size: int) -> bytes:
        for name, vaddr, vsize, raw_size, raw_ptr in self.sections:
            span = max(vsize, raw_size)
            if vaddr <= rva < vaddr + span:
                delta = rva - vaddr
                if delta + size > raw_size:
                    raise RepackError(
                        f"{self.path}: {name} RVA 0x{rva:X}+0x{size:X} is not raw")
                start = raw_ptr + delta
                return self.data[start:start + size]
        raise RepackError(f"{self.path}: RVA 0x{rva:X} is outside every section")


def sevenzip_list(sevenzip: Path, archive: Path) -> tuple[int, int]:
    result = subprocess.run(
        [str(sevenzip), "l", "-slt", str(archive)],
        capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RepackError(f"7z l failed: {result.stdout}{result.stderr}")
    size = None
    crc = None
    for line in result.stdout.splitlines():
        if line.startswith("Size = "):
            size = int(line.split("=", 1)[1].strip())
        elif line.startswith("CRC = "):
            crc = int(line.split("=", 1)[1].strip(), 16)
    if size is None or crc is None:
        raise RepackError("7z l did not report Size/CRC")
    return size, crc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path,
                        help="decrypted client image")
    parser.add_argument("--manifest", required=True, type=Path,
                        help="build.json produced by offsetdump")
    parser.add_argument("--out", type=Path,
                        help="payload.bin output (default build/repack/<build>.bin)")
    parser.add_argument("--archive", type=Path,
                        help="also write a 7z archive here (builds/.../payload.7z)")
    parser.add_argument("--sevenzip", type=Path,
                        default=Path("tools/7zip/7z.exe"),
                        help="pinned 7z CLI (default tools/7zip/7z.exe)")
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    build = manifest["build"]
    loader = manifest["loader"]
    expected = manifest["payload"]

    text_rva = int(loader["textRva"])
    text_size = int(loader["textSize"])
    unwind_rva = int(loader["runtime"])
    unwind_count = int(loader["rtCount"])
    unwind_size = unwind_count * 12

    exe = PE(args.exe)
    text = exe.slice(text_rva, text_size)
    unwind = exe.slice(unwind_rva, unwind_size)
    payload = text + struct.pack("<Q", unwind_count) + unwind

    size = len(payload)
    digest = sha256(payload)
    if size != int(expected["size"]):
        raise RepackError(
            f"payload size {size} != manifest {expected['size']}")
    if digest != expected["sha256"]:
        raise RepackError(
            f"payload sha256 {digest} != manifest {expected['sha256']}")

    out = args.out or Path("build") / "repack" / f"{build}.bin"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(payload)
    print(f"payload  {size} bytes 0x{size:X}")
    print(f"sha256   {digest}")
    print(f"out      {out}")

    if args.archive:
        if not args.sevenzip.exists():
            raise RepackError(f"missing 7z CLI at {args.sevenzip}")
        args.archive.parent.mkdir(parents=True, exist_ok=True)
        args.archive.unlink(missing_ok=True)
        with tempfile.TemporaryDirectory(prefix="zend-repack-") as temp:
            temp_dir = Path(temp)
            staged = temp_dir / "payload.bin"
            staged.write_bytes(payload)
            staged_archive = temp_dir / "payload.7z"
            command = [
                str(args.sevenzip), "a", "-t7z",
                "-m0=BCJ2", "-m1=LZMA2:d256m:fb273",
                "-m2=LZMA2:d64k", "-m3=LZMA2:d64k",
                "-mb0:1", "-mb0s1:2", "-mb0s2:3",
                "-mx=9", "-mmt=on",
                "-mtc=off", "-mta=off", "-mtm=off",
                "payload.7z", "payload.bin",
            ]
            result = subprocess.run(command, capture_output=True, text=True,
                                    cwd=temp_dir, check=False)
            if result.returncode != 0:
                raise RepackError(f"7z a failed: {result.stdout}{result.stderr}")
            archive_size, archive_crc = sevenzip_list(args.sevenzip, staged_archive)
            payload_crc = zlib.crc32(payload) & 0xFFFFFFFF
            if archive_size != size or archive_crc != payload_crc:
                raise RepackError(
                    f"archive entry mismatch: size {archive_size}/{size}, "
                    f"crc {archive_crc:08X}/{payload_crc:08X}")
            shutil.move(str(staged_archive), str(args.archive))
        print(f"archive  {args.archive} ({args.archive.stat().st_size} bytes)")
        print(f"crc32    {archive_crc:08X} ok")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RepackError as error:
        print(f"repack: {error}", file=sys.stderr)
        raise SystemExit(1)
