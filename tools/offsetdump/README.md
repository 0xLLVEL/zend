# offsetdump

Builds the per-build manifest from a decrypted client image and the dumper's
generated headers.

## Inputs

| File | Purpose |
|---|---|
| `RobloxPlayerBeta_<build>_decrypted.exe` | full decrypted image, source of sections/imports |
| `offsets.hpp` | Credoxy RVA dump, second opinion for core offsets |
| `hyperion.hpp` | loader profile: image/entry, hashes, 899 imports, patch sites |
| `hyperion_win_live_client_<build>.txt` | Hyperion live RVA dump, canonical core offsets |

The packed live `RobloxPlayerBeta.exe` is optional and only used to check
`SizeOfImage` and `AddressOfEntryPoint`.

## Usage

```powershell
python tools/offsetdump/offsetdump.py `
  --exe path\to\RobloxPlayerBeta_<build>_decrypted.exe `
  --live path\to\version-<build>\RobloxPlayerBeta.exe `
  --profile path\to\hyperion.hpp `
  --offsets path\to\offsets.hpp `
  --client-dump path\to\hyperion_win_live_client_<build>.txt `
  --payload path\to\payload.bin
```

Output goes to `builds/version-<build>/`:

- `build.json` — one manifest for loader and core, plus the payload hash/size.
- `loader.offsets.json` — header, sections, all import slots, TLS and patch sites.
- `core.offsets.json` — the RVAs `module.dll` consumes.

## Checks

- The build id from the exe name must match `hyperion.hpp`.
- `.pdata` count/size must match the profile, and the entry thunk is followed
  to the real client main (`epNew`).
- Every profile import slot must exist in the decrypted image's import table.
- Core offsets are read twice (Hyperion dump + Credoxy) where both exist.
- When `--payload` is passed, the payload rebuilt from `.text` + `.pdata` must
  match byte-for-byte. This is how the known-good 0.747 payload is verified.

Any mismatch exits non-zero. The tool never writes a manifest with a warning.
