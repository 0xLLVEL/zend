#!/usr/bin/env node
// F7 live check: filesystem helpers and crypt.
//
//   node tests/f7-filesystem-crypt.mjs [--pid <pid>]
//
// The filesystem half also inspects the real workspace directory from Node, so
// a passing Lua roundtrip cannot hide a path that landed outside the sandbox.

import fs from "node:fs";
import net from "node:net";
import path from "node:path";

const VERSION = 1;
const TIMEOUT_MS = 15000;
const EXEC_TIMEOUT_MS = 20000;

const args = process.argv.slice(2);
const pidIndex = args.indexOf("--pid");
const onlyPid = pidIndex >= 0 ? Number(args[pidIndex + 1]) : null;

function newestSession() {
  const dir = path.join(process.env.LOCALAPPDATA, "zend", "runtime");
  const files = fs
    .readdirSync(dir)
    .filter((name) => name.startsWith("session-") && name.endsWith(".json"))
    .filter((name) => onlyPid === null || name === `session-${onlyPid}.json`)
    .map((name) => path.join(dir, name))
    .sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs);

  if (files.length === 0) throw new Error(`no session file in ${dir}`);
  return JSON.parse(fs.readFileSync(files[0], "utf8"));
}

const session = newestSession();
const pipe = session.pipe.startsWith("\\\\.\\pipe\\")
  ? session.pipe
  : `\\\\.\\pipe\\${session.pipe}`;

const workspace = path.join(process.env.LOCALAPPDATA, "zend", "workspace");
const testFolder = path.join(workspace, "f7");

const socket = net.connect(pipe);
let buffer = Buffer.alloc(0);
let nextId = 1;
const waiting = new Map();

function send(message) {
  const payload = Buffer.from(JSON.stringify({ v: VERSION, ...message }), "utf8");
  const header = Buffer.alloc(4);
  header.writeUInt32LE(payload.length, 0);
  socket.write(Buffer.concat([header, payload]));
}

function request(message) {
  const id = nextId++;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      waiting.delete(id);
      reject(new Error(`timeout waiting for ${message.op} reply`));
    }, TIMEOUT_MS);

    waiting.set(id, (reply) => {
      clearTimeout(timer);
      waiting.delete(id);
      resolve(reply);
    });
    send({ id, ...message });
  });
}

function exec(source) {
  return request({ op: "exec", source, timeout_ms: EXEC_TIMEOUT_MS });
}

socket.on("data", (chunk) => {
  buffer = Buffer.concat([buffer, chunk]);

  while (buffer.length >= 4) {
    const length = buffer.readUInt32LE(0);
    if (buffer.length < 4 + length) return;

    const body = buffer.subarray(4, 4 + length);
    buffer = buffer.subarray(4 + length);

    let message;
    try {
      message = JSON.parse(body.toString("utf8"));
    } catch {
      continue;
    }

    const handler = waiting.get(message.id);
    if (
      handler &&
      (message.op === "result" || message.op === "pong" || message.op === "status.result")
    ) {
      handler(message);
    }
  }
});

socket.on("error", (err) => {
  console.error(`f7-check: ${err.message}`);
  process.exit(1);
});

let failures = 0;

function check(name, condition, detail) {
  if (condition) {
    console.log(`ok   ${name}`);
  } else {
    failures += 1;
    console.log(`FAIL ${name}`);
    if (detail !== undefined) console.log(`     ${JSON.stringify(detail)}`);
  }
}

function lines(result) {
  const text = String(result?.output ?? "").trim();
  return text.length === 0 ? [] : text.split("\n");
}

async function expect(label, source, expected) {
  const result = await exec(source);
  const actual = lines(result);
  check(
    label,
    result.ok && expected.every((line, i) => actual[i] === line),
    { ok: result.ok, output: result.output, error: result.error },
  );
  return actual;
}

socket.on("connect", async () => {
  try {
    console.log(`f7-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    await expect(
      "clean start",
      `pcall(delfolder, "f7")
print(isfolder("f7"), isfile("f7"))`,
      ["false\tfalse"],
    );

    await expect(
      "writefile creates parent folders and readfile roundtrips",
      `writefile("f7/deep/a.txt", "hello")
print(isfolder("f7/deep"), isfile("f7/deep/a.txt"), readfile("f7/deep/a.txt"))`,
      ["true\ttrue\thello"],
    );

    check(
      "file landed in the Node-side workspace",
      fs.existsSync(path.join(testFolder, "deep", "a.txt")) &&
        fs.readFileSync(path.join(testFolder, "deep", "a.txt"), "utf8") === "hello",
      { workspace },
    );

    await expect(
      "appendfile appends",
      `appendfile("f7/deep/a.txt", " world")
writefile("f7/deep/b.txt", "")
print(readfile("f7/deep/a.txt"), #readfile("f7/deep/b.txt"))`,
      ["hello world\t0"],
    );

    await expect(
      "binary data roundtrips",
      `local bytes = string.char(0, 1, 2, 250, 255) .. "mid" .. string.char(0)
writefile("f7/deep/binary.bin", bytes)
local back = readfile("f7/deep/binary.bin")
print(#back, back == bytes, string.byte(back, 1), string.byte(back, 4))`,
      ["9\ttrue\t0\t250"],
    );

    await expect(
      "listfiles lists relative entries",
      `makefolder("f7/empty")
local entries = listfiles("f7")
local deep, empty = false, false
for _, entry in ipairs(entries) do
  if string.find(entry, "deep$") then deep = true end
  if string.find(entry, "empty$") then empty = true end
end
local inner = listfiles("f7/deep")
local a, binary = false, false
for _, entry in ipairs(inner) do
  if string.find(entry, "a%.txt$") then a = true end
  if string.find(entry, "binary%.bin$") then binary = true end
end
print(type(entries), deep, empty, a, binary)`,
      ["table\ttrue\ttrue\ttrue\ttrue"],
    );

    await expect(
      "listfiles on a missing folder errors",
      `print((pcall(listfiles, "f7/missing")))`,
      ["false"],
    );

    await expect(
      "delfile removes a file",
      `delfile("f7/deep/b.txt")
print(isfile("f7/deep/b.txt"), (pcall(delfile, "f7/deep/b.txt")))`,
      ["false\tfalse"],
    );

    await expect(
      "delfolder removes nested contents",
      `delfolder("f7")
print(isfolder("f7"), isfile("f7/deep/a.txt"))`,
      ["false\tfalse"],
    );

    check(
      "folder is gone on disk too",
      !fs.existsSync(testFolder),
      { testFolder },
    );

    await expect(
      "readfile on a missing file errors",
      `print((pcall(readfile, "f7/nope.txt")))`,
      ["false"],
    );

    await expect(
      "path traversal is rejected",
      `print(
  (pcall(readfile, "../zend-escape.txt")),
  (pcall(writefile, "../zend-escape.txt", "x")),
  (pcall(makefolder, "../zend-escape")),
  (pcall(readfile, "f7/../../zend-escape.txt"))
)`,
      ["false\tfalse\tfalse\tfalse"],
    );

    await expect(
      "absolute and device paths are rejected",
      `local target = "C:/zend-escape.txt"
print(
  (pcall(writefile, target, "x")),
  (pcall(readfile, "C:/Windows/win.ini")),
  (pcall(writefile, "//./C:/zend-escape.txt", "x")),
  (pcall(readfile, "\\\\?\\C:\\Windows\\win.ini"))
)`,
      ["false\tfalse\tfalse\tfalse"],
    );

    await expect(
      "blocked extensions are rejected",
      `print(
  (pcall(writefile, "f7/evil.exe", "x")),
  (pcall(appendfile, "f7/evil.DLL", "x"))
)`,
      ["false\tfalse"],
    );

    await expect(
      "the workspace root cannot be deleted",
      `print((pcall(delfolder, "")), (pcall(delfolder, ".")))`,
      ["false\tfalse"],
    );

    await expect(
      "base64 known vectors",
      `print(base64encode("hello"), base64encode(""), base64encode(string.char(0, 1, 2)))`,
      ["aGVsbG8=\t\tAAEC"],
    );

    await expect(
      "base64decode roundtrips binary",
      `local raw = string.char(0, 1, 2, 250, 255)
print(base64decode(base64encode(raw)) == raw, base64_decode("aGVsbG8="), base64_encode("hi"))`,
      ["true\thello\taGk="],
    );

    await expect(
      "invalid base64 errors",
      `print((pcall(base64decode, "abc")), (pcall(base64decode, "!!!!")))`,
      ["false\tfalse"],
    );

    await expect(
      "base64 and crypt.base64 tables work",
      `print(
  type(base64.encode), type(base64.decode),
  base64.encode("zend"), base64.decode("emVuZA=="),
  crypt.base64.encode("zend"), crypt.base64.decode("emVuZA==")
)`,
      ["function\tfunction\temVuZA==\tzend\temVuZA==\tzend"],
    );

    await expect(
      "crypt.hash known vectors",
      `print(
  crypt.hash("abc", "md5"),
  crypt.hash("abc", "sha1"),
  crypt.hash("abc", "sha224")
)`,
      [
        "900150983cd24fb0d6963f7d28e17f72\ta9993e364706816aba3e25717850c26c9cd0d89d\t23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7",
      ],
    );

    await expect(
      "crypt.hash sha2 vectors",
      `print(
  crypt.hash("abc", "sha256"),
  crypt.hash("abc", "sha384"),
  crypt.hash("abc", "sha512")
)`,
      [
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\tcb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7\tddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
      ],
    );

    await expect(
      "crypt.hash sha3 and default",
      `print(
  crypt.hash("abc", "sha3-256"),
  crypt.hash("abc", "sha3_512") == crypt.hash("abc", "sha3-512"),
  #crypt.hash("abc")
)`,
      [
        "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532\ttrue\t96",
      ],
    );

    await expect(
      "unsupported hash algorithm errors",
      `print((pcall(crypt.hash, "abc", "crc32")))`,
      ["false"],
    );

    await expect(
      "generatekey returns a base64 256-bit key",
      `local key = crypt.generatekey()
print(type(key), type(base64decode(key)), #base64decode(key))`,
      ["string\tstring\t32"],
    );

    await expect(
      "encrypt and decrypt roundtrip",
      `local key = crypt.generatekey()
local iv = base64encode(string.rep("i", 16))
local plain = "zend-crypt-roundtrip"
local encrypted, usedIv = crypt.encrypt(plain, key, iv)
local decrypted = crypt.decrypt(encrypted, key, usedIv)
print(encrypted ~= plain, usedIv == iv, decrypted == plain, #base64decode(encrypted) % 16)`,
      ["true\ttrue\ttrue\t0"],
    );

    await expect(
      "encrypt generates an IV when omitted",
      `local key = crypt.generatekey()
local encrypted, iv = crypt.encrypt("payload", key)
print(type(encrypted), #base64decode(iv), crypt.decrypt(encrypted, key, iv))`,
      ["string\t16\tpayload"],
    );

    await expect(
      "invalid key, IV and mode error",
      `local key = crypt.generatekey()
print(
  (pcall(crypt.encrypt, "x", "not-a-key")),
  (pcall(crypt.encrypt, "x", key, base64encode("short"))),
  (pcall(crypt.encrypt, "x", key, nil, "ECB"))
)`,
      ["false\tfalse\tfalse"],
    );

    await expect(
      "crypt survives a yield",
      `task.wait(0.1)
print(base64encode("yield"), crypt.hash("abc", "md5"))`,
      ["eWllbGQ=\t900150983cd24fb0d6963f7d28e17f72"],
    );

    console.log(failures === 0 ? "f7-check: pass" : `f7-check: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  } catch (err) {
    console.error(`f7-check: ${err.message}`);
    process.exit(1);
  }
});
