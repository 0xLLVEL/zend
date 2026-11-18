# ZEND protocol v1

Transport: Windows named pipe `\\.\pipe\zend-<nonce>`.

Framing: `uint32` little-endian payload length, then UTF-8 JSON. Maximum frame is
16 MiB; a larger frame drops the connection. One request/response pair per `id`;
events carry no `id`.

Discovery: `module.dll` writes `%LOCALAPPDATA%\zend\runtime\session-<pid>.json`
atomically on startup and deletes it on unload. The UI watches that directory.

```json
{
  "v": 1,
  "pid": 12345,
  "pipe": "zend-6f1c...",
  "token": "base64",
  "build": "4ea8e85481e34f46",
  "ts": 1760000000
}
```

The token is not a security boundary against same-user processes; it prevents
accidental cross-wiring. The real boundary is the pipe ACL plus
`FILE_FLAG_FIRST_PIPE_INSTANCE`. The DACL grants full control to SYSTEM, the
object owner, and the interactive user SID explicitly; the pipe carries a
Medium mandatory label so a non-elevated UI can attach to an elevated client
(High integrity). Low-integrity processes cannot write to it.

## Operations

| op | direction | fields |
|---|---|---|
| `hello` | UI → core | `token`, `pid` |
| `exec` | UI → core | `source`, `chunk` |
| `result` | core → UI | `ok`, `output`, `error`, `ms` |
| `status` | UI → core | — |
| `status.result` | core → UI | `build`, `place_id`, `queue`, `heat`, `attached` |
| `log` | core → UI | `level`, `msg`, `ts` |
| `toast` | core → UI | `title`, `content`, `kind` |
| `ping` | either | — |
| `pong` | either | — |
| `detach` | UI → core | — |

`log` and `toast` are events, not replies. `result` and `status.result` echo the
request `id`.

`exec` runs the chunk on the core's persistent Luau thread and replies with
`result`. The mock core returns a successful echo; `pipe-check.mjs` accepts a
core that has not wired execution yet when run with `--allow-no-exec`.

See `protocol.ts` and `protocol.hpp` for the generated shapes, and
`protocol.schema.json` for validation.
