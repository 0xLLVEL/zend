# repack

Rebuilds the loader payload from a decrypted client image and checks it against
the manifest.

## Layout

```
.text VirtualSize bytes
uint64 LE .pdata entry count
.pdata (count * 12) bytes
```

Section padding is trimmed, so the output is the same regardless of how the
dumper aligned `.text` in the file. For 0.747 this is 110,374,844 bytes
(`0x6942FBC`), SHA-256
`2d27024c6fa1cba76cf0487f2509be988aff07e7e106641b36c795a7dc0619df`.

## Usage

```powershell
python tools/repack/repack.py `
  --exe path\to\RobloxPlayerBeta_<build>_decrypted.exe `
  --manifest builds\version-<build>\build.json `
  --archive builds\version-<build>\payload.7z
```

`--out` defaults to `build/repack/<build>.bin`. `--archive` is optional.

## Guarantees

- Size and SHA-256 must equal `build.json`; a mismatch is an error.
- The archive is always built from a staged file named `payload.bin`, with
  timestamps and attributes disabled, using the same BCJ2 + LZMA2 switches as
  the loader CMake. Two runs produce byte-identical archives.
- After packing, the tool reads the archive entry back through `7z l -slt` and
  compares its size and CRC-32 with the payload in memory.
