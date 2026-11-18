"""Disassemble one RVA window of a PE image with capstone.

    python tools/disasm_rva.py <image> <rva-hex> [--back <bytes>] [--context <insns>]

Finds a plausible function start by scanning backwards for inter-function
padding, then disassembles forward past the requested RVA.
"""

import argparse
import struct

from capstone import CS_ARCH_X86, CS_MODE_64, Cs


def load_sections(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE image")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + opt_size
    sections = []
    for i in range(count):
        off = table + i * 40
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, max(vsize, raw_size), raw_ptr, raw_size))
    return data, sections


def rva_to_offset(sections, rva):
    for vaddr, span, raw_ptr, raw_size in sections:
        if vaddr <= rva < vaddr + span:
            delta = rva - vaddr
            if delta >= raw_size:
                return None
            return raw_ptr + delta
    return None


def find_function_start(data, sections, rva, limit):
    start = rva
    offset = rva_to_offset(sections, rva)
    if offset is None:
        return rva
    lowest = max(0, offset - limit)
    scan = offset - 1
    while scan > lowest:
        if data[scan] == 0xCC and data[scan - 1] == 0xCC:
            while scan > lowest and data[scan] == 0xCC:
                scan -= 1
            if data[scan] in (0xC3, 0xC2):
                return rva - (offset - (scan + 1))
            break
        scan -= 1
    return rva


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("rva")
    parser.add_argument("--back", type=lambda v: int(v, 0), default=0x2000)
    parser.add_argument("--context", type=int, default=48)
    args = parser.parse_args()

    data, sections = load_sections(args.image)
    rva = int(args.rva, 0)
    start = find_function_start(data, sections, rva, args.back)
    offset = rva_to_offset(sections, start)
    if offset is None:
        raise SystemExit("rva is not backed by file data")

    print(f"function start rva 0x{start:X} (offset 0x{offset:X})")

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False

    seen = 0
    for insn in md.disasm(data[offset:offset + 0x6000], start):
        marker = "???" if insn.address == rva else "   "
        print(f"{marker} 0x{insn.address:08X}  {insn.mnemonic:<8} {insn.op_str}")
        if insn.address >= rva:
            seen += 1
            if seen > args.context:
                break


if __name__ == "__main__":
    main()
