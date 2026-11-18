# RobloxPlayerBeta.dll (loader)

Proxy DLL for the pinned 0.747 build (`4ea8e85481e34f46`). It replaces the
stock loader in the version directory, restores the decrypted client image from
an embedded 7z resource, patches imports, and redirects the client entry point.
It then loads `module.dll` (the core) from the same directory.

Ported from the reference Hyperion loader.

## Boot path

1. `DllMain` validates the live exe header: `SizeOfImage == 0x94C2000`,
   `AddressOfEntryPoint == 0x94C0004`.
2. `init()` extracts `IDR_PAYLOAD` (payload 7z) with `Mem7zExtract`, restores
   `.text` (`0x066167DC` bytes) and `.pdata` (`0x43B52` entries), fixes section
   flags, patches 900 imports, applies the Luau bytecode patch, registers the
   function table, and redirects the entry point to `entry()`.
3. The client enters `entry()` → `prep()` (PEB scrub, TLS calls, hooks, VEH,
   `lod` thread) → `ep()` → real client entry at `0x05FCBF1C`.
4. `lod` waits 5 s, `LoadLibraryW(<game dir>\module.dll)`, then unlinks the
   module from the PEB (`kStealth`).

## Fail-open

A pristine copy of the stock loader is deployed as
`RobloxPlayerBeta.stock.dll` next to the proxy.

| Failure point | Result |
|---|---|
| Header mismatch | Load stock DLL, return `TRUE`, forward `run` to its export |
| `init()` fails before `restore()` commits | Same fail-open |
| `init()` fails after `restore()` | Fatal (`DLL_PROCESS_INIT_FAILED`); the image is already patched |

The stock DLL is loaded **inside `DllMain`**, before returning to the exe. This
is intentional: the tempest entry page is `int3` until the loader writes the
entry jump, so waiting for `run()` would crash the client first. The stock
`DllMain` performs the same work it would have done during import resolution,
and the proxy's exported `run` forwards to the stock `run` afterwards.

## Build

```powershell
.\build.ps1
```

Output: `build/loader/Release/RobloxPlayerBeta.dll`.

- Payload input: a per-build `payload.bin` (110,374,844 bytes for 0.747), or
  override `-DZEND_PAYLOAD_BIN=<path>`.
- `tools/7zip/7z.exe` packs it as BCJ2 + LZMA2 into an RC resource
  (`IDR_PAYLOAD 101`). The archive is cached; it only repacks when the payload
  changes.
- Missing payload → fail-open-only build (used by CI). The proxy still builds
  and links but cannot boot the client.
- Static CRT (`/MT`), links `bcrypt iphlpapi ws2_32 WindowsApp`.

## Logs

`%LOCALAPPDATA%\zend\logs\loader.log`, one line per trace, behind
`kTrace = true` in `include/config/config.h`. `detlog` stays off
(`kDebugLog = false`); turning it on adds a `detlog.jsonl` and a shared section.

## Known gaps (inherited from the reference)

`kPng`, `kProxy`/`kMux`, and `kEpNew` cave selection are not active in the 0.747
boot path. These are inherited from the reference and are not used by the 0.747
launch path.
