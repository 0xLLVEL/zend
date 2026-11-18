# Tools

- `7zip/` — pinned 7-Zip 22.01 CLI + DLL used by the loader build to pack the
  payload resource. License in `7zip/license.txt`.
- `import-audit.ps1` - forbidden-import audit for `module.dll`. Fails on forbidden
  imports; allowlists the two Kernel32 imports the static CRT needs
  (`VirtualProtect`, `RtlLookupFunctionEntry`) and source-greps the core so a
  direct call still fails. Also rejects `module.dll` / `RobloxPlayerBeta.dll`
  strings in the core binary.
- `offsetdump/` — build `build.json`, `loader.offsets.json`, and
  `core.offsets.json` from a decrypted client image plus the dumper headers.
  Cross-checks imports, entry thunk, `.pdata`, and core RVAs; see its README.
- `repack/` — rebuild `payload.bin` from decrypted `.text` + `.pdata`, assert
  size/hash against the manifest, and pack a deterministic `payload.7z`.
- `detlog/` — read the loader's `DET1` shared section (`Local\RobloxDetLog_<pid>`)
  or `detlog.jsonl`, print heat and recent events, optionally follow. Only
  present when `kDebugLog` is on; `tests/detlog-selftest.py` covers the reader
  with a synthetic ring.
- `soak/` — sample a live session (`status` queue/ready/hooked plus DET1 heat)
  into JSONL; `tests/soak-check.mjs` covers it with the mock core.
