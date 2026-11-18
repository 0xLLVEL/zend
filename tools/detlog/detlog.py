#!/usr/bin/env python3
"""Read the loader's DET1 shared section and print detection events.

`module.dll` publishes a fixed-size shared section per client process:

    Local\\RobloxDetLog_<pid>

  header  48 bytes: magic 'DET1', version, capacity, head, image base,
                    start tick, heat, drop, pid
  events  4096 x 64 bytes, overwritten in place as a ring

It also appends the same events as JSON lines to `detlog.jsonl` in the
configured log directory (usually `%TEMP%`). This tool reads either source.

The section only exists when the loader was built with `kDebugLog`. The
reader never writes to the section; it is safe to run against a live client.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import struct
import sys
import time
from ctypes import wintypes
from pathlib import Path

kHeader = struct.Struct("<IIIIQQIIII")
kEvent = struct.Struct("<QQQQQQIHBBII")
kMagic = 0x31544544  # 'DET1'
kVersion = 1
kMaxEvents = 1 << 20

kSources = {0: "Unknown", 1: "Hyperion", 2: "Luau", 3: "Emulator"}
kKinds = {
    0: "None", 1: "Resolve", 2: "MemQuery", 3: "ProtChange",
    4: "HandleQuery", 5: "ModuleEnum", 6: "ProcOpen", 7: "ThreadEnum",
    8: "DebugCheck", 9: "TimingProbe", 10: "HwidRead", 11: "PdataCheck",
    12: "ServiceStub", 13: "ServiceUnknown", 14: "PatchApplied",
    15: "PatchFailed", 16: "PatchReverted", 17: "SelfSeen",
    18: "ChallengeFail", 19: "CallbackMiss", 20: "TimingImplausible",
    21: "DebugInfo", 22: "StackWalk", 23: "EnvAccess", 24: "MetaAccess",
    25: "IdentityCheck", 26: "ErrorLeak",
}
kVerdicts = {0: "Observed", 1: "Suspicious", 2: "Confirmed"}

kFILE_MAP_READ = 0x0004
kTH32CS_SNAPPROCESS = 0x00000002
kINVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenFileMappingW.restype = wintypes.HANDLE
kernel32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
kernel32.MapViewOfFile.restype = ctypes.c_void_p
kernel32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                   wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]


kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]


def processes(exe_name: str | None = None) -> list[tuple[int, str]]:
    snapshot = kernel32.CreateToolhelp32Snapshot(kTH32CS_SNAPPROCESS, 0)
    if not snapshot or snapshot == kINVALID_HANDLE_VALUE:
        return []
    rows: list[tuple[int, str]] = []
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    try:
        ok = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            name = entry.szExeFile
            if exe_name is None or name.lower() == exe_name.lower():
                rows.append((int(entry.th32ProcessID), name))
            ok = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return rows


def section_name(pid: int) -> str:
    return f"Local\\RobloxDetLog_{pid}"


def open_section(pid: int) -> tuple[int, int] | None:
    """Return (mapping handle, view pointer) or None if the section is absent."""
    handle = kernel32.OpenFileMappingW(kFILE_MAP_READ, False, section_name(pid))
    if not handle:
        return None
    view = kernel32.MapViewOfFile(handle, kFILE_MAP_READ, 0, 0, 0)
    if not view:
        kernel32.CloseHandle(handle)
        return None
    return handle, view


def close_section(handle: int, view: int) -> None:
    kernel32.UnmapViewOfFile(view)
    kernel32.CloseHandle(handle)


def read_bytes(view: int, size: int) -> bytes:
    return ctypes.string_at(view, size)


def decode_event(raw: bytes, index: int, base: int) -> dict:
    (tick, caller, a0, a1, a2, a3, tid, kind, source, verdict,
     seq, _pad) = kEvent.unpack(raw)
    return {
        "index": index,
        "tick": tick,
        "seq": seq,
        "tid": tid,
        "kind": kKinds.get(kind, f"Kind{kind}"),
        "source": kSources.get(source, f"Source{source}"),
        "verdict": kVerdicts.get(verdict, f"Verdict{verdict}"),
        "caller": caller,
        "rva": caller - base if base and caller >= base else 0,
        "a0": a0, "a1": a1, "a2": a2, "a3": a3,
    }


def read_section(pid: int, limit: int) -> dict | None:
    opened = open_section(pid)
    if opened is None:
        return None
    handle, view = opened
    try:
        raw = read_bytes(view, kHeader.size)
        magic, version, capacity, head, base, start, heat, drop, spid, _pad = \
            kHeader.unpack(raw)
        if magic != kMagic:
            return {"error": f"bad magic 0x{magic:08X} (expected DET1)"}
        if version != kVersion:
            return {"error": f"unsupported version {version}"}
        capacity = min(capacity, kMaxEvents)
        total = min(head, capacity)
        first = max(0, head - limit) if limit else head
        events = []
        for seq in range(first, head):
            offset = kHeader.size + (seq % capacity) * kEvent.size
            events.append(decode_event(
                ctypes.string_at(view + offset, kEvent.size), seq, base))
        return {
            "pid": spid or pid,
            "version": version,
            "capacity": capacity,
            "events": total,
            "head": head,
            "heat": heat,
            "drop": drop,
            "base": base,
            "start": start,
            "rows": events,
        }
    finally:
        close_section(handle, view)


def pick_pid(requested: int | None, exe_name: str) -> int | None:
    if requested:
        return requested
    candidates = [pid for pid, _ in processes(exe_name)]
    for pid in candidates:
        probe = read_section(pid, 0)
        if probe is not None:
            return pid
    return None


def format_event(row: dict, delta: str = "") -> str:
    caller = f"0x{row['rva']:X}" if row["rva"] else f"0x{row['caller']:X}"
    args = " ".join(f"a{i}=0x{row[f'a{i}']:X}"
                    for i in range(4) if row[f"a{i}"])
    return (f"{delta}#{row['seq']:<6} {row['kind']:<16} "
            f"{row['verdict']:<10} {row['source']:<8} tid={row['tid']:<6} "
            f"at={caller:<12} {args}").rstrip()


kHexKeys = {"caller", "rva", "a0", "a1", "a2", "a3"}


def as_document(row: dict) -> dict:
    return {key: (f"0x{value:X}" if key in kHexKeys else value)
            for key, value in row.items() if key != "index"}


def print_report(state: dict, as_json: bool) -> None:
    rows = state["rows"]
    if as_json:
        for row in rows:
            print(json.dumps(as_document(row)))
        return

    freq = ctypes.c_longlong()
    kernel32.QueryPerformanceFrequency(ctypes.byref(freq))
    scale = freq.value or 1
    origin = rows[0]["tick"] if rows else 0

    print(f"pid {state['pid']}  base 0x{state['base']:X}  "
          f"heat {state['heat']}/100  events {state['events']}  "
          f"drop {state['drop']}  ring {state['capacity']}")
    for row in rows:
        delta = f"{(row['tick'] - origin) / scale:7.3f}s " if origin else ""
        print(format_event(row, delta))


def summarize(rows: list[dict], as_json: bool = False) -> None:
    counts: dict[str, int] = {}
    for row in rows:
        counts[row["kind"]] = counts.get(row["kind"], 0) + 1
    ranked = sorted(counts.items(), key=lambda item: (-item[1], item[0]))
    if as_json:
        print(json.dumps({"kinds": dict(ranked)}))
        return
    print("kinds: " + ", ".join(f"{name}x{count}" for name, count in ranked))


def read_log(path: Path, limit: int) -> list[dict]:
    if not path.exists():
        return []
    rows: list[dict] = []
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                document = json.loads(line)
            except json.JSONDecodeError:
                continue
            if document.get("snapshot"):
                continue
            row = {
                "seq": int(document.get("seq", 0)),
                "tick": int(document.get("t", 0)),
                "tid": int(document.get("tid", 0)),
                "kind": document.get("kind", "?"),
                "source": document.get("src", "?"),
                "verdict": document.get("verdict", "?"),
                "caller": int(document.get("caller", "0x0"), 16),
                "rva": int(document.get("rva", "0x0"), 16),
                "a0": int(document.get("a0", "0x0"), 16),
                "a1": int(document.get("a1", "0x0"), 16),
                "a2": int(document.get("a2", "0x0"), 16),
                "a3": int(document.get("a3", "0x0"), 16),
                "heat": int(document.get("heat", 0)),
            }
            rows.append(row)
    return rows[-limit:]


def default_log_path() -> Path:
    import tempfile
    return Path(tempfile.gettempdir()) / "detlog.jsonl"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pid", type=int, help="client process id")
    parser.add_argument("--process", default="RobloxPlayerBeta.exe",
                        help="image name used when --pid is omitted")
    parser.add_argument("--log", type=Path,
                        help="detlog.jsonl to read instead of shared memory "
                             f"(default when the section is absent: {default_log_path()})")
    parser.add_argument("--list", action="store_true",
                        help="list client processes that expose a DET1 section")
    parser.add_argument("-n", "--show", type=int, default=40,
                        help="events to print (default 40)")
    parser.add_argument("--json", action="store_true", help="one JSON object per event")
    parser.add_argument("--summary", action="store_true",
                        help="print a count per event kind")
    parser.add_argument("--follow", action="store_true",
                        help="poll the section and print new events")
    parser.add_argument("--interval", type=float, default=1.0,
                        help="poll interval in seconds for --follow")
    args = parser.parse_args()

    if args.list:
        found = False
        for pid, name in processes(args.process):
            state = read_section(pid, 0)
            if state is None or "error" in state:
                continue
            found = True
            print(f"{pid:7} {name:<28} heat {state['heat']:>3}/100 "
                  f"events {state['events']:>6} drop {state['drop']}")
        if not found:
            print("no DET1 section found (is the client running with kDebugLog?)")
        return 0

    log_path = args.log
    pid = args.pid

    if pid is None and log_path is None:
        pid = pick_pid(None, args.process)
        if pid is None:
            log_path = default_log_path()
            print(f"detlog: no DET1 section; reading {log_path}", file=sys.stderr)

    if log_path is not None:
        rows = read_log(log_path, args.show)
        if not rows:
            print(f"detlog: no events in {log_path}", file=sys.stderr)
            return 1
        if args.json:
            for row in rows:
                print(json.dumps(as_document(row)))
            return 0
        origin = rows[0]["tick"]
        print(f"log {log_path}  heat {rows[-1].get('heat', 0)}/100  "
              f"events {len(rows)}")
        for row in rows:
            print(format_event(row, f"{(row['tick'] - origin) / 1000:7.3f}s "))
        if args.summary:
            summarize(rows, args.json)
        return 0

    state = read_section(pid, args.show)
    if state is None:
        print(f"detlog: pid {pid} has no {section_name(pid)}", file=sys.stderr)
        return 1
    if "error" in state:
        print(f"detlog: {state['error']}", file=sys.stderr)
        return 1

    print_report(state, args.json)
    if args.summary:
        summarize(state["rows"], args.json)

    if args.follow:
        seen = state["head"]
        try:
            while True:
                time.sleep(args.interval)
                state = read_section(pid, 0)
                if state is None or "error" in state:
                    print("detlog: section closed", file=sys.stderr)
                    return 0
                if state["head"] <= seen:
                    continue
                fresh = read_section(pid, state["head"] - seen)
                if not fresh or "error" in fresh:
                    continue
                for row in fresh["rows"]:
                    if row["seq"] >= seen:
                        print(format_event(row))
                seen = state["head"]
        except KeyboardInterrupt:
            return 0

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError as error:
        print(f"detlog: {error}", file=sys.stderr)
        raise SystemExit(1)
