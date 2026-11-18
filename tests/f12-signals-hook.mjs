#!/usr/bin/env node
// F12 live check: getsignalarguments, getsignalargumentsinfo, hooksignal,
// restoresignal and issignalhooked (debUNC contract).
//
//   node tests/f12-signals-hook.mjs [--pid <pid>]

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
  console.error(`f12-check: ${err.message}`);
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
    console.log(`f12-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    await expect(
      "getsignalarguments reports the engine signature types",
      `local part = Instance.new("Part")
local touched = getsignalarguments(part.Touched)
print(#touched, touched[1])
local added = getsignalarguments(workspace.ChildAdded)
print(#added, added[1])
local humanoid = Instance.new("Humanoid")
print(#getsignalarguments(humanoid.Died), #getsignalarguments(humanoid.Destroying))
local input = getsignalarguments(game:GetService("UserInputService").InputBegan)
print(#input, input[1], input[2])
part:Destroy()
humanoid:Destroy()`,
      ["1\tInstance", "1\tInstance", "0\t0", "2\tInstance\tBool"],
    );

    await expect(
      "getsignalargumentsinfo reports argument names and types",
      `local info = getsignalargumentsinfo(Instance.new("Part").Touched)
print(#info, info[1].Name, info[1].Type)
local input = getsignalargumentsinfo(game:GetService("UserInputService").InputBegan)
print(#input, input[2].Name, input[2].Type)
local added = getsignalargumentsinfo(workspace.ChildAdded)
print(#added, added[1].Type, type(added[1].Name))
print(#getsignalargumentsinfo(Instance.new("Humanoid").Destroying))`,
      [
        "1\totherPart\tInstance",
        "2\tgameProcessedEvent\tBool",
        "1\tInstance\tstring",
        "0",
      ],
    );

    await expect(
      "hooksignal calls the hook with Info and the fired values",
      `local event = Instance.new("BindableEvent")
local calls = {}
local seen = {}
local original = function(value) table.insert(calls, "original:" .. tostring(value)) end
event.Event:Connect(original)
hooksignal(event.Event, function(Info, value)
    table.insert(calls, "hook:" .. tostring(value))
    seen.Function = Info.Function == original
    seen.Connection = type(Info.Connection) == "table"
    seen.Index = Info.Index
    return true
end)
print(issignalhooked(event.Event))
event:Fire("x")
task.wait(0.15)
print(#calls, calls[1], calls[2])
print(seen.Function, seen.Connection, seen.Index)
restoresignal(event.Event)
print(issignalhooked(event.Event))`,
      ["true", "2\thook:x\toriginal:x", "true\ttrue\t0", "false"],
    );

    await expect(
      "returning false stops the connection from firing",
      `local event = Instance.new("BindableEvent")
local hits = 0
event.Event:Connect(function() hits += 1 end)
hooksignal(event.Event, function(Info) return false end)
event:Fire()
task.wait(0.1)
print(hits)
restoresignal(event.Event)
print(issignalhooked(event.Event))
event:Fire()
task.wait(0.1)
print(hits)`,
      ["0", "false", "1"],
    );

    await expect(
      "getconnections keeps reporting the original function while hooked",
      `local event = Instance.new("BindableEvent")
local original = function() end
event.Event:Connect(original)
hooksignal(event.Event, function() return true end)
local hooked = getconnections(event.Event)
print(#hooked, hooked[1].Function == original, hooked[1].LuaConnection, hooked[1].Enabled)
restoresignal(event.Event)
local restored = getconnections(event.Event)
print(#restored, restored[1].Function == original)`,
      ["1\ttrue\ttrue\ttrue", "1\ttrue"],
    );

    await expect(
      "a hook only affects its own signal instance",
      `local first = Instance.new("Part")
local second = Instance.new("Part")
local hits = 0
first.Touched:Connect(function() hits += 100 end)
second.Touched:Connect(function() hits += 1 end)
hooksignal(first.Touched, function(Info) return false end)
firesignal(first.Touched)
firesignal(second.Touched)
task.wait(0.15)
print(hits)
restoresignal(first.Touched)
firesignal(first.Touched)
task.wait(0.15)
print(hits)
first:Destroy()
second:Destroy()`,
      ["1", "101"],
    );

    await expect(
      "hooks give the newest connection index 0",
      `local event = Instance.new("BindableEvent")
local order = {}
local older = function() end
local newer = function() end
event.Event:Connect(older)
event.Event:Connect(newer)
hooksignal(event.Event, function(Info)
    if Info.Function == newer then
        table.insert(order, "newer:" .. tostring(Info.Index))
    elseif Info.Function == older then
        table.insert(order, "older:" .. tostring(Info.Index))
    end
    return true
end)
local connections = getconnections(event.Event)
print(connections[1].Function == newer, connections[2].Function == older)
event:Fire()
task.wait(0.15)
table.sort(order)
print(#order, order[1], order[2])
restoresignal(event.Event)`,
      ["true\ttrue", "2\tnewer:0\tolder:1"],
    );

    await expect(
      "a hook may yield before letting the original run",
      `local event = Instance.new("BindableEvent")
local seen = 0
event.Event:Connect(function(value) seen += 1 end)
hooksignal(event.Event, function(Info, value)
    task.wait(0.05)
    return true
end)
event:Fire("x")
task.wait(0.25)
print(seen)
restoresignal(event.Event)`,
      ["1"],
    );

    await expect(
      "hooksignal on an empty signal can be restored",
      `local event = Instance.new("BindableEvent")
print(issignalhooked(event.Event))
hooksignal(event.Event, function() return false end)
print(issignalhooked(event.Event))
restoresignal(event.Event)
print(issignalhooked(event.Event))`,
      ["false", "true", "false"],
    );

    await expect(
      "invalid arguments are rejected cleanly",
      `local event = Instance.new("BindableEvent")
print((pcall(hooksignal)))
print((pcall(hooksignal, "signal", function() end)))
print((pcall(hooksignal, event.Event)))
print((pcall(restoresignal, 1)))
print((pcall(issignalhooked, 1)))
print((pcall(getsignalarguments, 1)))
print((pcall(getsignalargumentsinfo, 1)))`,
      [
        "false",
        "false",
        "false",
        "false",
        "false",
        "false",
        "false",
      ],
    );
  } catch (err) {
    failures += 1;
    console.log(`FAIL ${err.message}`);
  } finally {
    console.log(failures === 0 ? "f12: all checks passed" : `f12: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  }
});
