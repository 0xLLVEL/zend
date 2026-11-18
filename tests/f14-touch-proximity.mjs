#!/usr/bin/env node
// F14 live check: firetouchinterest (sUNC toggle mapping) and
// fireproximityprompt.
//
//   node tests/f14-touch-proximity.mjs [--pid <pid>]

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
  console.error(`f14-check: ${err.message}`);
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
    console.log(`f14-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    await expect(
      "touch helpers are registered",
      `print(type(firetouchinterest), type(fireproximityprompt))`,
      ["function\tfunction"],
    );

    await expect(
      "0/1 starts and ends a touch exactly once per part",
      `local first = Instance.new("Part")
local second = Instance.new("Part")
for _, part in {first, second} do
    part.Anchored = true
    part.CanCollide = false
    part.CanTouch = true
    part.Size = Vector3.new(1, 1, 1)
end
first.Position = Vector3.new(0, 500, 0)
second.Position = Vector3.new(0, 505, 0)
first.Parent = workspace
second.Parent = workspace
task.wait(0.2)
local touched = {0, 0}
local ended = {0, 0}
first.Touched:Connect(function(other)
    if other == second then touched[1] += 1 end
end)
second.Touched:Connect(function(other)
    if other == first then touched[2] += 1 end
end)
first.TouchEnded:Connect(function(other)
    if other == second then ended[1] += 1 end
end)
second.TouchEnded:Connect(function(other)
    if other == first then ended[2] += 1 end
end)
task.wait(0.2)
firetouchinterest(first, second, 0)
task.wait(0.4)
print(touched[1], touched[2], ended[1], ended[2])
firetouchinterest(first, second, 1)
task.wait(0.4)
print(touched[1], touched[2], ended[1], ended[2])
first:Destroy()
second:Destroy()`,
      ["1\t1\t0\t0", "1\t1\t1\t1"],
    );

    await expect(
      "true/false follow the sUNC touch/un-touch mapping",
      `local function makePair(x)
    local first = Instance.new("Part")
    local second = Instance.new("Part")
    for _, part in {first, second} do
        part.Anchored = true
        part.CanCollide = false
        part.CanTouch = true
        part.Size = Vector3.new(1, 1, 1)
    end
    first.Position = Vector3.new(x, 500, 0)
    second.Position = Vector3.new(x, 505, 0)
    first.Parent = workspace
    second.Parent = workspace
    return first, second
end
local first, second = makePair(20)
local touched = {0, 0}
local ended = {0, 0}
first.Touched:Connect(function() touched[1] += 1 end)
second.Touched:Connect(function() touched[2] += 1 end)
first.TouchEnded:Connect(function() ended[1] += 1 end)
second.TouchEnded:Connect(function() ended[2] += 1 end)
task.wait(0.2)
firetouchinterest(first, second, false)
task.wait(0.3)
print(touched[1], touched[2])
first:Destroy()
second:Destroy()

first, second = makePair(30)
touched = {0, 0}
ended = {0, 0}
first.Touched:Connect(function() touched[1] += 1 end)
second.Touched:Connect(function() touched[2] += 1 end)
first.TouchEnded:Connect(function() ended[1] += 1 end)
second.TouchEnded:Connect(function() ended[2] += 1 end)
task.wait(0.2)
firetouchinterest(first, second, true)
task.wait(0.4)
print(touched[1], touched[2], ended[1], ended[2])
firetouchinterest(first, second, false)
task.wait(0.4)
print(touched[1], touched[2], ended[1], ended[2])
first:Destroy()
second:Destroy()`,
      ["0\t0", "1\t1\t0\t0", "1\t1\t1\t1"],
    );

    await expect(
      "fireproximityprompt triggers the prompt for the local player",
      `local part = Instance.new("Part")
part.Anchored = true
part.CanCollide = false
part.Size = Vector3.new(2, 2, 2)
part.Position = Vector3.new(40, 500, 0)
part.Parent = workspace
local prompt = Instance.new("ProximityPrompt")
prompt.Enabled = true
prompt.RequiresLineOfSight = false
prompt.MaxActivationDistance = 32
prompt.HoldDuration = 0
prompt.Parent = part
local hits = 0
local player = nil
prompt.Triggered:Connect(function(who)
    hits += 1
    player = who
end)
task.wait(0.2)
print(fireproximityprompt(prompt) == nil)
task.wait(0.4)
print(hits, player == game:GetService("Players").LocalPlayer)
part:Destroy()`,
      ["true", "1\ttrue"],
    );

    await expect(
      "invalid arguments are rejected cleanly",
      `local part = Instance.new("Part")
local humanoid = Instance.new("Humanoid")
print((pcall(firetouchinterest)))
print((pcall(firetouchinterest, part)))
print((pcall(firetouchinterest, part, "second")))
print((pcall(firetouchinterest, humanoid, part)))
print((pcall(fireproximityprompt)))
print((pcall(fireproximityprompt, "prompt")))
print((pcall(fireproximityprompt, part)))
part:Destroy()
humanoid:Destroy()`,
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

    await expect(
      "touch calls still work after a yield",
      `local first = Instance.new("Part")
local second = Instance.new("Part")
for _, part in {first, second} do
    part.Anchored = true
    part.CanCollide = false
    part.CanTouch = true
    part.Size = Vector3.new(1, 1, 1)
end
first.Position = Vector3.new(60, 500, 0)
second.Position = Vector3.new(60, 505, 0)
first.Parent = workspace
second.Parent = workspace
task.wait(0.1)
local hits = 0
first.Touched:Connect(function() hits += 1 end)
firetouchinterest(first, second, 0)
task.wait(0.1)
firetouchinterest(first, second, 1)
task.wait(0.3)
print(hits)
first:Destroy()
second:Destroy()`,
      ["1"],
    );
  } catch (err) {
    failures += 1;
    console.log(`FAIL ${err.message}`);
  } finally {
    console.log(failures === 0 ? "f14: all checks passed" : `f14: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  }
});
