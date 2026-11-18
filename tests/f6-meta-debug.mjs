#!/usr/bin/env node
// F6 live check: metatable, closure and debug helpers.
//
//   node tests/f6-meta-debug.mjs [--pid <pid>]
//
// Every chunk runs through the executor (task.defer), so the helpers are also
// exercised from the scheduler coroutine the client creates for a chunk.

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
  console.error(`f6-check: ${err.message}`);
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
    console.log(`f6-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    await expect(
      "getrawmetatable ignores __metatable",
      `local t = setmetatable({}, {
  __metatable = "locked",
  __index = function() return 1 end,
})
local mt = getrawmetatable(t)
print(type(mt), mt.__metatable, getmetatable(t) == mt, t.foo)`,
      ["table\tlocked\ttrue\t1"],
    );

    await expect(
      "setrawmetatable replaces the metatable",
      `local t = {}
local mt = {__index = function() return 42 end}
print(setrawmetatable(t, mt) == t, t.anything, getrawmetatable(t) == mt)`,
      ["true\t42\ttrue"],
    );

    await expect(
      "setmetatable is the raw variant",
      `local t = {}
print(setmetatable(t, {__index = function() return 9 end}) == t, t.x)`,
      ["true\t9"],
    );

    await expect(
      "setreadonly, isreadonly and make_writeable",
      `local t = {x = 1}
setreadonly(t, true)
local ok = pcall(function() t.y = 2 end)
local ro = isreadonly(t)
make_writeable(t)
t.y = 2
print(ok, ro, isreadonly(t), t.y)`,
      ["false\ttrue\tfalse\t2"],
    );

    await expect(
      "closure predicates",
      `print(
  iscclosure(print), islclosure(function() end),
  iscclosure(function() end), islclosure(print),
  iscclosure(3), islclosure(nil), checkcaller()
)`,
      ["true\ttrue\tfalse\tfalse\tfalse\tfalse\tfalse"],
    );

    await expect(
      "clonefunction copies a Lua closure and shares upvalues",
      `local x = 5
local orig = function(a) return a + x end
local copy = clonefunction(orig)
x = 7
print(copy(1), islclosure(copy), copy ~= orig)`,
      ["8\ttrue\ttrue"],
    );

    await expect(
      "clonefunction copies a C closure",
      `local copy = clonefunction(print)
print(iscclosure(copy), copy ~= print)`,
      ["true\ttrue"],
    );

    await expect(
      "getinfo describes a function",
      `local f = function(a, b) return a + b end
local info = getinfo(f)
print(
  type(info), info.func == f, info.what, info.source,
  info.linedefined >= 0, info.numparams, info.nups
)`,
      ["table\ttrue\tLua\t@zend\ttrue\t2\t0"],
    );

    await expect(
      "getinfo describes a stack level",
      `local here = getinfo(1)
print(here.source, type(here.currentline), here.currentline > 0)`,
      ["@zend\tnumber\ttrue"],
    );

    await expect(
      "getconstants lists literal constants",
      `local f = function(a) return 1.25, "hello", a, {} end
local k = getconstants(f)
local nums, strs = 0, 0
for _, v in ipairs(k) do
  if type(v) == "number" and v == 1.25 then nums += 1 end
  if v == "hello" then strs += 1 end
end
print(type(k), nums, strs)`,
      ["table\t1\t1"],
    );

    await expect(
      "getupvalues reads captured values",
      `local a, b = 10, 20
local f = function() return a + b end
a = 11
b = 21
local u = getupvalues(f)
local sum = 0
for _, v in ipairs(u) do sum += v end
print(type(u), #u, sum)`,
      ["table\t2\t32"],
    );

    await expect(
      "setupvalue writes a captured value",
      `local a, b = 10, 20
local f = function() return a + b end
a = 11
b = 21
local u = getupvalues(f)
for i, v in ipairs(u) do
  if v == 11 then setupvalue(f, i, 100) end
end
print(f())`,
      ["121"],
    );

    await expect(
      "getproto returns a child proto as a function",
      `local parent = function() return function(x) return x * 2, "child" end end
local child = getproto(parent, 1)
local k = getconstants(child)
local has = false
for _, v in ipairs(k) do if v == 2 then has = true end end
print(type(child), islclosure(child), getinfo(child).what, has)`,
      ["function\ttrue\tLua\ttrue"],
    );

    await expect(
      "getstack exposes the caller frame",
      `local marker = "stack-marker-7"
local s = getstack(1)
local found = false
for _, v in ipairs(s) do
  if v == marker then found = true end
end
print(type(s), #s > 0, found)`,
      ["table\ttrue\ttrue"],
    );

    await expect(
      "debug table carries the helpers",
      `print(
  type(debug.getinfo), type(debug.getconstants), type(debug.getupvalues),
  type(debug.setupvalue), type(debug.getproto), type(debug.getstack),
  type(debug.info), type(debug.traceback)
)`,
      ["function\tfunction\tfunction\tfunction\tfunction\tfunction\tfunction\tfunction"],
    );

    await expect(
      "debug.traceback still works",
      `print(type(debug.traceback("f6")))`,
      ["string"],
    );

    await expect(
      "helpers survive a yield",
      `task.wait(0.1)
print(type(getrawmetatable({})), islclosure(function() end), getinfo(1).source)`,
      ["nil\ttrue\t@zend"],
    );

    console.log(failures === 0 ? "f6-check: pass" : `f6-check: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  } catch (err) {
    console.error(`f6-check: ${err.message}`);
    process.exit(1);
  }
});
