#!/usr/bin/env node
// F4 live check: chunks yield through the client's task scheduler.
//
//   node tests/f4-check.mjs [--pid <pid>]
//
// A chunk is handed to task.defer, so task.wait / pcall yields / task.defer
// inside it are legal. The reply arrives only when the chunk really finishes.

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
  console.error(`f4-check: ${err.message}`);
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

socket.on("connect", async () => {
  try {
    console.log(`f4-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    const waited = await exec('print("before") task.wait(0.35) print("after")');
    check("task.wait completes", waited.ok, waited.error);
    check("output spans the yield", waited.output.trim() === "before\nafter", waited.output);
    check("reply waits for the script", Number(waited.ms) >= 300, { ms: waited.ms });

    const throughPcall = await exec(
      'local ok, value = pcall(function() task.wait(0.2) return 7 end) print(ok, value)',
    );
    check(
      "yield passes through pcall",
      throughPcall.ok && throughPcall.output.trim() === "true\t7",
      { ok: throughPcall.ok, output: throughPcall.output, error: throughPcall.error },
    );

    const late = await exec('task.wait(0.15) error("late boom")');
    check(
      "error after a yield is reported",
      !late.ok && String(late.error).includes("late boom"),
      late.error,
    );
    check("late error carries a traceback", String(late.error).includes("zend"), late.error);

    const globals = await exec(
      'task.wait(0.05) print(typeof(game), game.PlaceId ~= nil and "place" or "none")',
    );
    check(
      "globals survive the yield",
      globals.ok && /^Instance\tplace/.test(globals.output.trim()),
      { ok: globals.ok, output: globals.output, error: globals.error },
    );

    const chain = await exec(
      'task.wait(0.1) task.wait(0.1) task.wait(0.1) print("three waits")',
    );
    check("repeated yields", chain.ok && chain.output.includes("three waits"), chain);
    check("three waits took at least 300 ms", Number(chain.ms) >= 300, { ms: chain.ms });

    const deferred = await exec(
      'task.defer(function() end) print("deferred ok")',
    );
    check("task.defer inside a chunk", deferred.ok, deferred.error);

    const after = await exec('print("still alive")');
    check(
      "next chunk runs after yields",
      after.ok && after.output.includes("still alive"),
      after.error,
    );

    console.log(failures === 0 ? "f4-check: pass" : `f4-check: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  } catch (err) {
    console.error(`f4-check: ${err.message}`);
    process.exit(1);
  }
});
