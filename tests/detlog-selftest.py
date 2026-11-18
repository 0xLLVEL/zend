#!/usr/bin/env python3
"""Exercise tools/detlog against a synthetic DET1 section.

The loader only publishes the section when built with `kDebugLog`, so this test
writes the same layout from Python and checks that the reader decodes it:
header fields, ring order, wrap-around, and the JSON/summary output.

Layout is duplicated here on purpose. If the C++ side changes, the static
asserts in `emulator/detlog.h` and this test fail together.
"""

from __future__ import annotations

import ctypes
import json
import struct
import subprocess
import sys
from ctypes import wintypes
from pathlib import Path

HEADER = struct.Struct("<IIIIQQIIII")
EVENT = struct.Struct("<QQQQQQIHBBII")
MAGIC = 0x31544544
CAPACITY = 8  # small ring so wrap-around is cheap to test

FILE_MAP_ALL_ACCESS = 0xF001F
PAGE_READWRITE = 0x04

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateFileMappingW.restype = wintypes.HANDLE
kernel32.CreateFileMappingW.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                                        wintypes.DWORD, wintypes.DWORD,
                                        wintypes.DWORD, wintypes.LPCWSTR]
kernel32.MapViewOfFile.restype = ctypes.c_void_p
kernel32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                   wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

root = Path(__file__).resolve().parent.parent
TOOL = root / "tools" / "detlog" / "detlog.py"


def event(seq: int, kind: int, source: int, verdict: int,
          caller: int, a0: int = 0) -> bytes:
    return EVENT.pack(1_000_000 + seq * 1000, caller, a0, 0, 0, 0,
                      4242, kind, source, verdict, seq, 0)


def main() -> int:
    name = f"Local\\RobloxDetLog_{ctypes.windll.kernel32.GetCurrentProcessId()}"
    base = 0x7FF600000000
    size = HEADER.size + CAPACITY * EVENT.size

    mapping = kernel32.CreateFileMappingW(
        None, None, PAGE_READWRITE, 0, size, name)
    if not mapping:
        raise OSError(ctypes.get_last_error(), "CreateFileMappingW failed")
    view = kernel32.MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size)
    if not view:
        kernel32.CloseHandle(mapping)
        raise OSError(ctypes.get_last_error(), "MapViewOfFile failed")

    pid = ctypes.windll.kernel32.GetCurrentProcessId()
    try:
        # Write CAPACITY+2 events: the first two are overwritten by the last two.
        total = CAPACITY + 2
        for seq in range(total):
            slot = seq % CAPACITY
            ctypes.memmove(view + HEADER.size + slot * EVENT.size,
                           event(seq, kind=1 + slot % 3, source=1,
                                 verdict=seq % 3, caller=base + 0x1000 * seq,
                                 a0=0xDEAD if seq == total - 1 else 0),
                           EVENT.size)

        header = HEADER.pack(MAGIC, 1, CAPACITY, total, base,
                             123456, 37, 5, pid, 0)
        ctypes.memmove(view, header, HEADER.size)

        result = subprocess.run(
            [sys.executable, str(TOOL), "--pid", str(pid), "-n", "4", "--json"],
            capture_output=True, text=True, check=False)
        if result.returncode != 0:
            print(result.stdout)
            print(result.stderr, file=sys.stderr)
            raise AssertionError(f"detlog exited {result.returncode}")

        rows = [json.loads(line) for line in result.stdout.splitlines() if line]
        seqs = [row["seq"] for row in rows]
        expected = list(range(total - 4, total))
        if seqs != expected:
            raise AssertionError(f"seq order {seqs}, expected {expected}")
        if rows[-1]["rva"] != f"0x{(0x1000 * (total - 1)):X}":
            raise AssertionError(f"rva {rows[-1]['rva']}")
        if rows[-1]["a0"] != "0xDEAD":
            raise AssertionError(f"a0 {rows[-1]['a0']}")

        summary = subprocess.run(
            [sys.executable, str(TOOL), "--pid", str(pid), "-n", "4", "--summary"],
            capture_output=True, text=True, check=False)
        if summary.returncode != 0 or "heat 37/100" not in summary.stdout:
            raise AssertionError(f"summary output missing heat:\n{summary.stdout}")
        if "kinds:" not in summary.stdout:
            raise AssertionError("summary output missing kind table")

        print("detlog-selftest: PASS "
              f"({len(rows)} events, seq {seqs[0]}..{seqs[-1]}, heat 37)")
        return 0
    finally:
        kernel32.UnmapViewOfFile(view)
        kernel32.CloseHandle(mapping)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"detlog-selftest: FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
