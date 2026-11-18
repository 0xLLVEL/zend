#!/usr/bin/env node
// Live streaming check: print/warn lines must reach the pipe while the chunk
// is still running, not only inside the final result.
//
//   node tests/stream-check.mjs [--pid <pid>]
//
// Covers: early output during a yield, warn kind, ordering, embedded newlines,
// a late print from task.defer after the result, and the complete result.output.
// A busy/background client may stop stepping its scheduler, so the late-print
// check wakes it with a trivial chunk when the line does not arrive on its own.

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

// Every output event seen, in arrival order, with a receive timestamp.
const streamed = [];
const outputWaiters = new Set();
// Per-request event log for the request that is currently running.
const open = new Map();

function send(message) {
  const payload = Buffer.from(JSON.stringify({ v: VERSION, ...message }), "utf8");
  const header = Buffer.alloc(4);
  header.writeUInt32LE(payload.length, 0);
  socket.write(Buffer.concat([header, payload]));
}

function waitForOutput(predicate, timeoutMs = TIMEOUT_MS) {
  const hit = streamed.find(predicate);
  if (hit) return Promise.resolve(hit);

  return new Promise((resolve, reject) => {
    const waiter = { predicate, resolve };
    outputWaiters.add(waiter);
    setTimeout(() => {
      outputWaiters.delete(waiter);
      reject(new Error("timeout waiting for streamed output"));
    }, timeoutMs);
  });
}

function exec(source, timeoutMs = TIMEOUT_MS) {
  const id = nextId++;
  return new Promise((resolve, reject) => {
    const entry = { id, lines: [], firstAt: null };
    open.set(id, entry);

    const timer = setTimeout(() => {
      waiting.delete(id);
      open.delete(id);
      reject(new Error(`timeout waiting for exec #${id} result`));
    }, timeoutMs);

    waiting.set(id, (message) => {
      if (message.op !== "result") return;
      clearTimeout(timer);
      waiting.delete(id);
      open.delete(id);
      resolve({
        ...message,
        lines: entry.lines,
        firstAt: entry.firstAt,
        resultAt: Date.now(),
      });
    });

    send({ id, op: "exec", source, timeout_ms: timeoutMs });
  });
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

    if (message.op === "output") {
      const event = { ...message, at: Date.now() };
      streamed.push(event);

      const entry = open.get(message.id);
      if (entry) {
        entry.lines.push(event);
        if (entry.firstAt === null) entry.firstAt = event.at;
      }

      for (const waiter of [...outputWaiters]) {
        if (waiter.predicate(event)) {
          outputWaiters.delete(waiter);
          waiter.resolve(event);
        }
      }
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
  console.error(`stream-check: ${err.message}`);
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

function texts(result) {
  return result.lines.map((line) => line.text);
}

socket.on("connect", async () => {
  try {
    console.log(`stream-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    // 1. Output during a yield must arrive before the result.
    const timing = await exec(
      'print("stream early")\ntask.wait(0.6)\nprint("stream late")',
    );
    check("streaming chunk succeeds", timing.ok, timing.error);
    check("both lines streamed", texts(timing).join("|") === "stream early|stream late", texts(timing));
    check("output precedes the result", timing.firstAt !== null && timing.resultAt - timing.firstAt >= 300, {
      firstAt: timing.firstAt,
      resultAt: timing.resultAt,
    });
    check(
      "result.output is still complete",
      timing.output.trim() === "stream early\nstream late",
      timing.output,
    );

    // 2. warn keeps its own kind.
    const warned = await exec('warn("streamed warning")');
    check("warn streams", texts(warned).join("|") === "streamed warning", texts(warned));
    check("warn kind preserved", warned.lines[0]?.kind === "warn", warned.lines[0]);
    check("warn lands in result.output", warned.output.includes("streamed warning"), warned.output);

    // 3. Multiple prints keep their order.
    const ordered = await exec('print("one")\nprint("two")\nprint("three")');
    check("prints stay ordered", texts(ordered).join("|") === "one|two|three", texts(ordered));

    // 4. An embedded newline becomes one event per line.
    const multiline = await exec('print("alpha\\nbeta")');
    check("embedded newline splits", texts(multiline).join("|") === "alpha|beta", texts(multiline));
    check(
      "multiline result.output intact",
      multiline.output.trim() === "alpha\nbeta",
      multiline.output,
    );

    // 5. Empty arguments still produce a line.
    const empty = await exec('print("")');
    check("empty print streams a blank line", empty.lines.length === 1 && empty.lines[0].text === "", texts(empty));

    // 6. A deferred coroutine's print arrives after the result. The client may
    //    pause or stall its script scheduler while nothing is dispatched (busy
    //    places, background throttling), so wake it with a trivial chunk if the
    //    line does not arrive on its own. No newer real dispatch runs in
    //    between, so the line still exercises the post-complete sink.
    const deferred = await exec(
      'print("defer start")\ntask.defer(function()\n  task.wait(0.4)\n  print("late print from defer")\nend)',
    );
    check("defer chunk returns promptly", deferred.ok && deferred.ms < 3500, {
      ok: deferred.ok,
      ms: deferred.ms,
      error: deferred.error,
    });
    check("late print is not in result.output", !deferred.output.includes("late print from defer"), deferred.output);

    const latePredicate = (event) =>
      event.kind === "print" && event.text === "late print from defer";
    let late = await waitForOutput(latePredicate, 700).catch(() => null);
    let wakeId = null;
    if (!late) {
      const wake = await exec('print("wake")');
      wakeId = wake.id;
      late = await waitForOutput(latePredicate, 5000);
    }
    check("late print streams after the result", late.at > deferred.resultAt, {
      resultAt: deferred.resultAt,
      lateAt: late.at,
    });
    check(
      "late print carries a request id",
      late.id === deferred.id || late.id === wakeId,
      { late: late.id, request: deferred.id, wake: wakeId },
    );

    console.log(failures === 0 ? "stream-check: pass" : `stream-check: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  } catch (err) {
    console.error(`stream-check: ${err.message}`);
    process.exit(1);
  }
});
