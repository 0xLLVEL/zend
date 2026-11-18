"""Disassemble a raw memory window dumped by the core's fault handler.

    python tools/disasm_bin.py <file> --base <rva-hex> --at <offset> [--context N]

Offset 0 of the file maps to --base. --at marks the faulting instruction.
With --auto-start the script searches for the earliest offset that disassembles
consistently onto the fault instruction and starts there.
"""

import argparse

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

MD = Cs(CS_ARCH_X86, CS_MODE_64)


def reaches(data, base, start, target):
    for insn in MD.disasm(data[start:], base + start):
        offset = insn.address - base
        if offset == target:
            return True
        if offset > target:
            return False
    return False


def find_start(data, base, target):
    best = None
    for start in range(0, target):
        if not reaches(data, base, start, target):
            continue
        if start > 0 and data[start - 1] in (0xCC, 0xC3, 0xC2):
            return start
        if best is None:
            best = start
    return best if best is not None else target


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file")
    parser.add_argument("--base", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--at", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--context", type=int, default=40)
    parser.add_argument("--auto-start", action="store_true")
    args = parser.parse_args()

    data = open(args.file, "rb").read()
    start = find_start(data, args.base, args.at) if args.auto_start else 0
    print(f"start +0x{start:04X} (rva 0x{args.base + start:X})")

    seen = 0
    for insn in MD.disasm(data[start:], args.base + start):
        offset = insn.address - args.base
        marker = "???" if offset == args.at else "   "
        print(f"{marker} +0x{offset:04X} 0x{insn.address:08X}  {insn.mnemonic:<8} {insn.op_str}")
        if offset >= args.at:
            seen += 1
            if seen > args.context:
                break


if __name__ == "__main__":
    main()
