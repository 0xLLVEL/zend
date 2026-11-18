#!/usr/bin/env node
// F13 live check: rconsoleprint/info/warn/err/clear and their sample aliases.
// Console output goes to a separate Win32 window, so this only asserts the Lua
// API surface, return values and error handling.
//
//   node tests/f13-rconsole.mjs [--pid <pid>]

import fs from "node:fs";
import net from "node:net";
import path from "node:path";

const VERSION = 1;
const TIMEOUT_MS = 15000;
const EXEC_TIMEOUT_MS = 30000;

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
      (message.op === "result" ||
        message.op === "pong" ||
        message.op === "status.result")
    ) {
      handler(message);
    }
  }
});

socket.on("error", (err) => {
  console.error(`f13-check: ${err.message}`);
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
    console.log(`f13-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    await expect(
      "rconsole functions and aliases are registered",
      `print(type(rconsoleprint), type(rconsoleinfo), type(rconsolewarn))
print(type(rconsoleerr), type(rconsoleclear))
print(type(consoleprint), type(consoleclear), type(rconsoleerror))`,
      ["function\tfunction\tfunction", "function\tfunction", "function\tfunction\tfunction"],
    );

    await expect(
      "rconsoleprint accepts strings, numbers and tables",
      `local before = rconsoleprint("plain text")
local number = rconsoleprint(42)
local table_value = rconsoleprint({1, 2})
rconsoleprint("@@RED@@red@@RESET@@ text", true, false)
print(before, number, table_value)
print(type(before), type(number), type(table_value))`,
      ["nil\tnil\tnil", "nil\tnil\tnil"],
    );

    await expect(
      "rconsoleinfo and rconsolewarn accept any value",
      `print(rconsoleinfo("info line") == nil)
print(rconsolewarn({key = "value"}) == nil)
consoleprint(7)`,
      ["true", "true"],
    );

    await expect(
      "rconsoleerr returns the message it printed",
      `local text = rconsoleerr("failure text")
local number = rconsoleerr(123)
local table_value = rconsoleerr({})
print(text, number)
print(type(table_value), table_value:sub(1, 5))`,
      ["failure text\t123", "string\ttable"],
    );

    await expect(
      "rconsoleclear clears the console and ignores arguments",
      `print(rconsoleclear() == nil)
print(pcall(rconsoleclear, "ignored", 1))
rconsoleprint("after clear")`,
      ["true", "true"],
    );

    await expect(
      "rconsole calls work across a yield",
      `rconsoleprint("before yield")
task.wait(0.05)
rconsoleinfo("after yield")
print("yield-ok")`,
      ["yield-ok"],
    );

    await expect(
      "rconsoleprint requires at least one argument",
      `print((pcall(rconsoleprint)))
print((pcall(rconsoleinfo)))
print((pcall(rconsolewarn)))
print((pcall(rconsoleerr)))`,
      ["false", "false", "false", "false"],
    );
  } catch (err) {
    failures += 1;
    console.log(`FAIL ${err.message}`);
  } finally {
    console.log(failures === 0 ? "f13: all checks passed" : `f13: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  }
});
