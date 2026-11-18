import struct
import sys

path = sys.argv[1]
data = open(path, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
count = struct.unpack_from("<H", data, pe + 6)[0]
opt_size = struct.unpack_from("<H", data, pe + 20)[0]
table = pe + 24 + opt_size

print(f"image size 0x{len(data):X} sections {count}")
for i in range(count):
    off = table + i * 40
    name = data[off:off + 8].rstrip(b"\0").decode(errors="replace")
    vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
    span = max(vsize, raw_size)
    print(
        f"{name:10} va=0x{vaddr:08X} vs=0x{vsize:08X} "
        f"raw=0x{raw_size:08X} at=0x{raw_ptr:08X} span_end=0x{vaddr + span:08X}"
    )
