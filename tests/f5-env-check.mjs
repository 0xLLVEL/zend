#!/usr/bin/env node
// F5 live check: environment v1 core subset.
//
//   node tests/f5-env-check.mjs [--pid <pid>]
//
// Every chunk runs through the executor (task.defer), so these checks also
// exercise the helpers from a scheduler coroutine, not the step thread.

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
  console.error(`f5-check: ${err.message}`);
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
    console.log(`f5-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    await expect(
      "getgenv returns our globals",
      `print(type(getgenv()), getgenv() == getfenv(0))`,
      ["table\ttrue"],
    );

    await expect(
      "_G aliases the executor globals",
      `_G.zend_env_marker = 123
print(_G == getgenv(), zend_env_marker)`,
      ["true\t123"],
    );

    await expect(
      "print shadow survives the helpers",
      `print(type(getgenv().print), getgenv().print == print)`,
      ["function\ttrue"],
    );

    await expect(
      "getrenv is the real environment, cached",
      `print(type(getrenv()), getrenv() == getrenv(), getrenv() ~= getgenv(),
  type(getrenv().print), getrenv().print ~= print, typeof(getrenv().game))`,
      ["table\ttrue\ttrue\tfunction\ttrue\tInstance"],
    );

    await expect(
      "getreg returns the registry",
      `local reg = getreg()
print(type(reg), reg == getreg(), type(reg._LOADED))`,
      ["table\ttrue\ttable"],
    );

    const gc = await exec(`
local fns = getgc()
local all = getgc(true)
print(type(fns), #fns > 0, #all >= #fns, type(fns[1]))
`);
    const gcLines = lines(gc);
    check(
      "getgc lists collectables",
      gc.ok && gcLines[0]?.startsWith("table\ttrue\ttrue\t"),
      { ok: gc.ok, output: gc.output, error: gc.error },
    );
    check(
      "getgc entries are GC objects",
      ["function", "thread", "userdata", "buffer"].includes(
        gcLines[0]?.split("\t")[3],
      ),
      gcLines[0],
    );
    check(
      "getgc(true) includes tables",
      gc.ok && gcLines[0]?.startsWith("table\ttrue\ttrue\t"),
      gcLines[0],
    );

    await expect(
      "getinstances returns parented instances and game",
      `local list = getinstances()
local clean, has_game = true, false
for _, v in ipairs(list) do
  if typeof(v) ~= "Instance" then clean = false end
  if v == game then has_game = true end
  if v ~= game and v.Parent == nil then clean = false end
end
print(#list > 0, clean, has_game)`,
      ["true\ttrue\ttrue"],
    );

    const nilInstances = await exec(`
local list = getnilinstances()
local clean = true
for _, v in ipairs(list) do
  if typeof(v) ~= "Instance" or v == game or v.Parent ~= nil then clean = false end
end
print(type(list), clean, #list)
`);
    const nilLines = lines(nilInstances);
    check(
      "getnilinstances excludes game and parented instances",
      nilInstances.ok && nilLines[0]?.startsWith("table\ttrue\t"),
      { ok: nilInstances.ok, output: nilInstances.output, error: nilInstances.error },
    );

    await expect(
      "getscripts returns client scripts",
      `local list = getscripts()
local clean = true
for _, v in ipairs(list) do
  local cn = v.ClassName
  local ok = cn == "LocalScript" or cn == "ModuleScript" or cn == "CoreScript" or cn == "Script"
  if typeof(v) ~= "Instance" or not ok then clean = false end
end
print(type(list), #list > 0, clean)`,
      ["table\ttrue\ttrue"],
    );

    await expect(
      "getcallingscript is nil for executor chunks",
      `print(type(getcallingscript()))`,
      ["nil"],
    );

    await expect(
      "loadstring compiles and runs",
      `local fn, err = loadstring("return 1 + 1")
print(type(fn), err == nil, fn())`,
      ["function\ttrue\t2"],
    );

    await expect(
      "loadstring reports compile errors",
      `local bad, msg = loadstring("return )")
print(bad == nil, type(msg))`,
      ["true\tstring"],
    );

    await expect(
      "loadstring runs in our globals",
      `local setter = loadstring("zend_loaded = 99")
setter()
print(zend_loaded)`,
      ["99"],
    );

    await expect(
      "loadstring functions may yield",
      `local fn = loadstring("task.wait(0.1) return 3")
print(fn())`,
      ["3"],
    );

    await expect(
      "environment survives a yield",
      `task.wait(0.1)
print(_G == getgenv(), type(getrenv()), getinstances()[1] ~= nil)`,
      ["true\ttable\ttrue"],
    );

    console.log(failures === 0 ? "f5-check: pass" : `f5-check: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  } catch (err) {
    console.error(`f5-check: ${err.message}`);
    process.exit(1);
  }
});
