# LZMA SDK (7z decoder)

Vendored LZMA SDK decoder for the loader's in-memory 7z extraction. The files
under `C/` are the LZMA SDK 7-Zip ANSI C decoder sources; `mem7z.c` / `mem7z.h`
add the in-memory `ILookInStream` used by the loader.

The LZMA SDK is public domain (Igor Pavlov, 7-Zip). See
<https://www.7-zip.org/sdk.html>. Only the subset needed for
`Z7_EXTRACT_ONLY` builds is compiled.
