#!/usr/bin/env node
// F11 live check: getconnections and firesignal.
//
//   node tests/f11-signals.mjs [--pid <pid>]

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
  console.error(`f11-check: ${err.message}`);
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
    console.log(`f11-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    await expect(
      "getconnections exists and returns an empty table for a fresh signal",
      `local event = Instance.new("BindableEvent")
local conns = getconnections(event.Event)
print(type(getconnections), type(get_signal_cons))
print(type(conns), #conns)
local alias = get_signal_cons(event.Event)
print(type(alias), #alias)
event:Destroy()`,
      ["function\tfunction", "table\t0", "table\t0"],
    );

    await expect(
      "a Luau connection exposes Enabled, Function, Thread and flags",
      `local event = Instance.new("BindableEvent")
local function handler(value) end
local native = event.Event:Connect(handler)
local conns = getconnections(event.Event)
print(#conns, typeof(conns[1]))
local c = conns[1]
print(c.Enabled, c.LuaConnection, c.ForeignState)
print(c.Function == handler, typeof(c.Thread))
native:Disconnect()
task.wait(0.1)
print(#getconnections(event.Event))
event:Destroy()`,
      [
        "1\ttable",
        "true\ttrue\tfalse",
        "true\tthread",
        "0",
      ],
    );

    await expect(
      "Function can be called directly and Fire passes arguments",
      `local event = Instance.new("BindableEvent")
local seen = {}
local function handler(...) table.insert(seen, { ... }) end
local native = event.Event:Connect(handler)
local c = getconnections(event.Event)[1]
c.Function("direct")
print(#seen, seen[1][1])
c:Fire("fired", 2)
print(#seen, seen[2][1], seen[2][2])
c:Defer("deferred")
print(#seen)
task.wait(0.1)
print(#seen, seen[3][1])
native:Disconnect()
event:Destroy()`,
      [
        "1\tdirect",
        "2\tfired\t2",
        "2",
        "3\tdeferred",
      ],
    );

    await expect(
      "Disable stops engine delivery and Enable restores it",
      `local event = Instance.new("BindableEvent")
local hits = 0
local function handler() hits += 1 end
local native = event.Event:Connect(handler)
local c = getconnections(event.Event)[1]
event:Fire()
task.wait(0.05)
print("first", hits)
c:Disable()
print("disabled", c.Enabled)
event:Fire()
task.wait(0.05)
print("after-disable", hits)
c:Enable()
print("enabled", c.Enabled)
event:Fire()
task.wait(0.05)
print("after-enable", hits)
native:Disconnect()
event:Destroy()`,
      [
        "first\t1",
        "disabled\tfalse",
        "after-disable\t1",
        "enabled\ttrue",
        "after-enable\t2",
      ],
    );

    await expect(
      "Disconnect through the connection object disables it",
      `local event = Instance.new("BindableEvent")
local native = event.Event:Connect(function() end)
local c = getconnections(event.Event)[1]
c:Disconnect()
print(c.Enabled, c.Function, c.Thread, c.LuaConnection)
local again = getconnections(event.Event)
print(#again, again[1].Enabled, again[1].Function ~= nil, again[1].LuaConnection)
event:Destroy()`,
      ["false\tnil\tnil\tfalse", "1\tfalse\ttrue\ttrue"],
    );

    await expect(
      "a native Disconnect removes the node from later snapshots",
      `local event = Instance.new("BindableEvent")
local keep = event.Event:Connect(function() end)
local drop = event.Event:Connect(function() end)
print(#getconnections(event.Event))
keep:Disconnect()
task.wait(0.2)
local conns = getconnections(event.Event)
print(#conns, conns[1].Enabled, conns[1].LuaConnection)
drop:Disconnect()
task.wait(0.2)
print(#getconnections(event.Event))
event:Destroy()`,
      ["2", "1\ttrue\ttrue", "0"],
    );

    await expect(
      "firesignal invokes every Luau connection with the given arguments",
      `local event = Instance.new("BindableEvent")
local seen = {}
local first = event.Event:Connect(function(...) table.insert(seen, "first") end)
local second = event.Event:Connect(function(...) table.insert(seen, "second") end)
firesignal(event.Event, 1, "two", true)
task.wait(0.1)
table.sort(seen)
print(#seen, table.concat(seen, ","))
local conns = getconnections(event.Event)
conns[1]:Disable()
seen = {}
firesignal(event.Event, "again")
task.wait(0.1)
table.sort(seen)
print(#seen, table.concat(seen, ","))
event:Destroy()

local plain = Instance.new("BindableEvent")
local left = plain.Event:Connect(function() table.insert(seen, "left") end)
local right = plain.Event:Connect(function() table.insert(seen, "right") end)
left:Disconnect()
right:Disconnect()
task.wait(0.2)
seen = {}
firesignal(plain.Event)
task.wait(0.1)
print(#seen)
plain:Destroy()`,
      ["2\tfirst,second", "2\tfirst,second", "0"],
    );

    await expect(
      "invalid signals are rejected cleanly",
      `print((pcall(getconnections)))
print((pcall(getconnections, 1)))
print((pcall(getconnections, Instance.new("Part"))))
print((pcall(firesignal, "signal")))`,
      ["false", "false", "false", "false"],
    );

    await expect(
      "signals and connections survive a yield",
      `local event = Instance.new("BindableEvent")
local hits = 0
local native = event.Event:Connect(function() hits += 1 end)
task.wait(0.05)
local c = getconnections(event.Event)[1]
print(c.Enabled, c.LuaConnection, typeof(c.Function))
c:Fire()
print(hits)
native:Disconnect()
event:Destroy()`,
      ["true\ttrue\tfunction", "1"],
    );
  } catch (err) {
    failures += 1;
    console.log(`FAIL ${err.message}`);
  } finally {
    console.log(failures === 0 ? "f11: all checks passed" : `f11: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  }
});
