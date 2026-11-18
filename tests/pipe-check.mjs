#!/usr/bin/env node
// Protocol smoke test against a running core (mock or real).
//
//   node tests/pipe-check.mjs [--pid <pid>] [--allow-no-exec]
//
// Finds the newest session file, says hello, pings, asks for status, and runs
// one script. --allow-no-exec accepts the honest F2 error from the real core;
// it becomes a hard failure once F3 lands.

import fs from "node:fs";
import net from "node:net";
import path from "node:path";

const VERSION = 1;
const TIMEOUT_MS = 5000;

const args = process.argv.slice(2);
const allowNoExec = args.includes("--allow-no-exec");
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
    if (handler && (message.op === "result" || message.op === "pong" || message.op === "status.result")) {
      handler(message);
    }
  }
});

socket.on("error", (err) => {
  console.error(`pipe-check: ${err.message}`);
  process.exit(1);
});

socket.on("connect", async () => {
  try {
    console.log(`pipe-check: ${pipe} · build ${session.build}`);

    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const pong = await request({ op: "ping" });
    console.log(`pong: id ${pong.id}`);

    const status = await request({ op: "status" });
    console.log(`status: build=${status.build} place=${status.place_id} queue=${status.queue}`);
    if (status.build !== session.build) {
      throw new Error(`build mismatch: ${status.build} != ${session.build}`);
    }

    const result = await request({
      op: "exec",
      source: 'print("hello from pipe-check")',
      chunk: "pipe-check",
    });

    console.log(`result: ok=${result.ok} ms=${result.ms}`);
    if (!result.ok) {
      if (!allowNoExec) throw new Error(`exec failed: ${result.error}`);
      console.log(`exec: unavailable as expected (${result.error})`);
    } else if (!String(result.output).includes("hello from pipe-check")) {
      throw new Error(`unexpected output: ${JSON.stringify(result.output)}`);
    }

    console.log("pipe-check: pass");
    socket.end();
    process.exit(0);
  } catch (err) {
    console.error(`pipe-check: ${err.message}`);
    process.exit(1);
  }
});
