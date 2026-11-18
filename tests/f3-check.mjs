#!/usr/bin/env node
// F3 live check: exec through the scheduler step hook against a running game.
//
//   node tests/f3-check.mjs [--pid <pid>]
//
// Runs five chunks: print capture, argument formatting, the Roblox global
// environment, a syntax error, and a runtime error with traceback.

import fs from "node:fs";
import net from "node:net";
import path from "node:path";

const VERSION = 1;
const TIMEOUT_MS = 8000;

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
  console.error(`f3-check: ${err.message}`);
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
    console.log(`f3-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    const print = await request({ op: "exec", source: 'print("hello from f3-check")' });
    check("print runs", print.ok, print.error);
    check("print captured", String(print.output).includes("hello from f3-check"), print.output);
    console.log(`     ms=${print.ms}`);

    const argsResult = await request({
      op: "exec",
      source: 'print("a", 1, true, nil)',
    });
    check("print formats arguments", argsResult.output.trim() === "a\t1\ttrue\tnil", argsResult.output);

    const env = await request({
      op: "exec",
      source: 'print(typeof(game), typeof(workspace), game.PlaceId)',
    });
    check(
      "roblox globals visible",
      env.ok && /^Instance\tInstance\t\d+/.test(env.output.trim()),
      {
        ok: env.ok,
        output: env.output,
        error: env.error,
      },
    );

    const syntax = await request({ op: "exec", source: "local = 1" });
    check("syntax error reported", !syntax.ok && /syntax|expected/i.test(syntax.error), syntax.error);

    const runtime = await request({ op: "exec", source: 'error("boom")' });
    check(
      "runtime error reported",
      !runtime.ok && String(runtime.error).includes("boom"),
      runtime.error,
    );
    check("traceback present", String(runtime.error).includes("zend"), runtime.error);

    console.log(failures === 0 ? "f3-check: pass" : `f3-check: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  } catch (err) {
    console.error(`f3-check: ${err.message}`);
    process.exit(1);
  }
});
