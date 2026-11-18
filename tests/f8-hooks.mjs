#!/usr/bin/env node
// F8 live check: hookfunction / restorefunction / newcclosure.
//
//   node tests/f8-hooks.mjs [--pid <pid>]
//
// Every case runs through the executor (task.defer), so the hooks also get
// exercised across the same yieldable C frames real scripts use.

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
  console.error(`f8-check: ${err.message}`);
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
    console.log(`f8-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    await expect(
      "Lua target keeps its identity and calls the Lua hook",
      `local calls = 0
local function f(x) return x + 1 end
local old
old = hookfunction(f, function(x) calls += 1 return old(x) * 10 end)
local a = f(1)
local b = old(41)
print(a, b, calls, isfunctionhooked(f), iscclosure(old), islclosure(old))
print(restorefunction(f), f(1), old(1), isfunctionhooked(f), restorefunction(f))`,
      ["20\t42\t1\ttrue\tfalse\ttrue", "true\t2\t2\tfalse\tfalse"],
    );

    await expect(
      "re-hooking returns the previous hook, restore keeps the first body",
      `local function f(x) return x end
local old1 = hookfunction(f, function(x) return x + 10 end)
local old2 = hookfunction(f, function(x) return x + 100 end)
print(f(1), old2(1), old1(1))
restorefunction(f)
print(f(1), isfunctionhooked(f))`,
      ["101\t11\t1", "1\tfalse"],
    );

    await expect(
      "C target with a Lua hook",
      `local old = hookfunction(math.floor, function() return 1000 end)
print(math.floor(1.9), old(1.9), iscclosure(old))
restorefunction(math.floor)
print(math.floor(1.9))`,
      ["1000\t1\ttrue", "1"],
    );

    await expect(
      "Lua target with a C hook",
      `local function f(x) return x end
local old = hookfunction(f, string.upper)
print(f("abc"), old("abc"), iscclosure(old))
restorefunction(f)
print(f("abc"))`,
      ["ABC\tabc\tfalse", "abc"],
    );

    await expect(
      "C target with a C hook",
      `local old = hookfunction(string.upper, string.lower)
print(string.upper("MiXeD"), old("MiXeD"))
restorefunction(string.upper)
print(string.upper("MiXeD"))`,
      ["mixed\tMIXED", "MIXED"],
    );

    await expect(
      "newcclosure wraps Lua functions and is reusable",
      `local x = 5
local f = function(a) return a + x end
local nc = newcclosure(f)
print(iscclosure(nc), islclosure(nc), nc(2), newcclosure(f) == nc, isnewcclosure(nc))
x = 7
print(nc(2), newcclosure(string.upper) == string.upper)`,
      ["true\tfalse\t7\ttrue\ttrue", "9\ttrue"],
    );

    await expect(
      "Lua target hooked with a newcclosure wrapper",
      `local function f(x) return x + 1 end
local nc = newcclosure(function(x) return x + 100 end)
local old = hookfunction(f, nc)
print(f(1), old(1), iscclosure(old))
restorefunction(f)
print(f(1))`,
      ["101\t2\tfalse", "2"],
    );

    await expect(
      "newcclosure target hooked with a Lua function and restored",
      `local function base(x) return x + 1000 end
local nc = newcclosure(base)
local old = hookfunction(nc, function(x) return x * 2 end)
print(nc(21), old(21), isnewcclosure(nc))
restorefunction(nc)
print(nc(21), isnewcclosure(nc), isfunctionhooked(nc))`,
      ["42\t1021\ttrue", "1021\ttrue\tfalse"],
    );

    await expect(
      "newcclosure target hooked with a C function",
      `local nc = newcclosure(function(s) return s .. "!" end)
local old = hookfunction(nc, string.upper)
print(nc("hi"), old("hi"), isnewcclosure(nc))
restorefunction(nc)
print(nc("hi"), isnewcclosure(nc))`,
      ["HI\thi!\ttrue", "hi!\ttrue"],
    );

    await expect(
      "C target hooked with a newcclosure wrapper",
      `local nc = newcclosure(function(x) return x * 3 end)
local old = hookfunction(math.abs, nc)
print(math.abs(-4), old(-4), iscclosure(old))
restorefunction(math.abs)
print(math.abs(-4))`,
      ["-12\t4\ttrue", "4"],
    );

    await expect(
      "a hook may yield and resume",
      `local function f() return "original" end
local old
old = hookfunction(f, function()
  task.wait(0.05)
  return old() .. "+hook"
end)
print(f())
restorefunction(f)
print(f())`,
      ["original+hook", "original"],
    );

    await expect(
      "newcclosure forwards yields",
      `local nc = newcclosure(function()
  task.wait(0.05)
  return "nc"
end)
print(nc())`,
      ["nc"],
    );

    await expect(
      "debug.info name survives hooking",
      `local function foo() return 1 end
local before = debug.info(foo, "n")
hookfunction(foo, function() return 2 end)
local after = debug.info(foo, "n")
print(before == after, after ~= nil, foo())
restorefunction(foo)`,
      ["true\ttrue\t2"],
    );

    await expect(
      "clonefunction of a hooked target keeps dispatching the hook",
      `local function f() return "orig" end
hookfunction(f, function() return "hook" end)
local copy = clonefunction(f)
print(copy())
restorefunction(f)
print(copy(), f())`,
      ["hook", "hook\torig"],
    );

    await expect(
      "hookfunction(target, old) uninstalls the hook",
      `local function f() return 1 end
local old = hookfunction(f, function() return 2 end)
print(f(), old())
hookfunction(f, old)
print(f(), isfunctionhooked(f), restorefunction(f))`,
      ["2\t1", "1\tfalse\tfalse"],
    );

    await expect(
      "nested Lua -> C -> Lua hook chain",
      `local function f(s) return s end
hookfunction(f, string.upper)
local oldUpper
oldUpper = hookfunction(string.upper, function(s) return "[" .. oldUpper(s) .. "]" end)
print(f("ab"), string.upper("cd"))
restorefunction(string.upper)
restorefunction(f)
print(f("ab"), string.upper("cd"))`,
      ["[AB]\t[CD]", "ab\tCD"],
    );

    await expect(
      "hookfunction rejects non-functions cleanly",
      `print((pcall(hookfunction, 1, function() end)))
print((pcall(hookfunction, function() end, 1)))
print((pcall(restorefunction, 1)))`,
      ["false", "false", "false"],
    );
  } catch (err) {
    failures += 1;
    console.log(`FAIL ${err.message}`);
  } finally {
    console.log(failures === 0 ? "f8: all checks passed" : `f8: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  }
});
