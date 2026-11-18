#!/usr/bin/env node
// F10 live check: identity helpers, instance helpers and getloadedmodules.
//
//   node tests/f10-identity-instance.mjs [--pid <pid>]

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
  console.error(`f10-check: ${err.message}`);
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
    console.log(`f10-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    await expect(
      "identity helpers exist under the usual names",
      `print(type(getidentity), type(getthreadidentity), type(getthreadcontext))
print(type(setidentity), type(setthreadidentity), type(setthreadcontext))
print(type(getcapabilities))`,
      [
        "function\tfunction\tfunction",
        "function\tfunction\tfunction",
        "function",
      ],
    );

    await expect(
      "executor threads start at identity 8",
      `print(getidentity())
local caps = getcapabilities()
print(type(caps), #caps > 0, caps.Identity, type(caps.Capabilities))`,
      ["8", "table\ttrue\t8\tnumber"],
    );

    await expect(
      "setidentity changes the value and survives a yield",
      `setidentity(2)
print(getidentity(), getthreadidentity())
task.wait(0.05)
print(getidentity())
setidentity(8)
print(getidentity(), getcapabilities().Identity)`,
      ["2\t2", "2", "8\t8"],
    );

    await expect(
      "identity aliases behave like the canonical names",
      `setthreadidentity(3)
print(getthreadcontext(), getcapabilities().Identity)
setthreadcontext(8)
print(getthreadidentity())
print((pcall(setidentity, "eight")))
print((pcall(setidentity)))`,
      ["3\t3", "8", "false", "false"],
    );

    await expect(
      "instance helpers walk a small tree",
      `local part = Instance.new("Part")
local a = Instance.new("Part"); a.Name = "A"; a.Parent = part
local folder = Instance.new("Folder"); folder.Name = "B"; folder.Parent = part
local nested = Instance.new("Part"); nested.Name = "Nested"; nested.Parent = folder
print(#getchildren(part), #getdescendants(part), typeof(getchildren(part)[1]))
print(findfirstchild(part, "A") == a)
print(findfirstchild(part, "Nested"), findfirstchild(part, "Nested", true) == nested)
print(findfirstchildofclass(part, "Folder") == folder, findfirstchildofclass(part, "Model"))
print(findfirstchildwhichisa(part, "Part", true) == a)
print(typeof(getpropertychanged(part, "Name")))
print((pcall(getchildren, 1)))
print((pcall(findfirstchild, part)))
print((pcall(findfirstchildofclass, part, {})))
part:Destroy()`,
      [
        "2\t3\tInstance",
        "true",
        "nil\ttrue",
        "true\tnil",
        "true",
        "RBXScriptSignal",
        "false",
        "false",
        "false",
      ],
    );

    await expect(
      "instance helpers work on live services",
      `print(#getchildren(workspace) > 0, #getdescendants(workspace) > 0)
print(findfirstchild(workspace, "Camera") ~= nil)
print(typeof(getpropertychanged(workspace, "Name")))`,
      ["true\ttrue", "true", "RBXScriptSignal"],
    );

    await expect(
      "getloadedmodules returns unique loaded ModuleScripts that getscripts sees",
      `local loaded = getloadedmodules()
local scripts = getscripts()
local known = {}
local moduleCount = 0
for _, script in ipairs(scripts) do
    if script.ClassName == "ModuleScript" then
        known[script] = true
        moduleCount = moduleCount + 1
    end
end

local seen = {}
local subset = true
local unique = true
local classOk = true
for _, module in ipairs(loaded) do
    if seen[module] then unique = false end
    seen[module] = true
    if known[module] == nil then subset = false end
    if typeof(module) ~= "Instance" or module.ClassName ~= "ModuleScript" then classOk = false end
end

print(type(loaded), classOk, unique, subset, #loaded <= moduleCount)
print(#getloadedmodules() == #loaded)`,
      ["table\ttrue\ttrue\ttrue\ttrue", "true"],
    );

    await expect(
      "identity and instance helpers still work after a yield",
      `setidentity(2)
task.wait(0.05)
local part = Instance.new("Part")
local child = Instance.new("Part"); child.Parent = part
print(getidentity(), #getchildren(part))
setidentity(8)
print(getidentity(), typeof(getloadedmodules()))`,
      ["2\t1", "8\ttable"],
    );
  } catch (err) {
    failures += 1;
    console.log(`FAIL ${err.message}`);
  } finally {
    console.log(failures === 0 ? "f10: all checks passed" : `f10: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  }
});
