#!/usr/bin/env node
// F8 soak sampler. Keeps one pipe connection open, asks `status` every interval,
// and records the answer plus the loader's DET1 heat as JSON lines.
//
//   node tools/soak/soak.mjs --hours 24
//   node tools/soak/soak.mjs --pid 23592 --minutes 10
//   node tools/soak/soak.mjs --samples 3 --interval 0.1 --no-heat
//
// Heat comes from `tools/detlog`; it is only available when the client runs a
// loader built with `-DZEND_DETLOG=ON`. Without the section the field stays
// null and the run continues.
//
// Exit codes: 0 clean, 1 budget/disconnect failure, 2 bad usage or no session.

import { spawn } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import path from "node:path";
import { fileURLToPath } from "node:url";

const VERSION = 1;
const REQUEST_TIMEOUT_MS = 8000;
const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..", "..");

function arg(name, fallback) {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
}

function flag(name) {
  return process.argv.includes(`--${name}`);
}

const pidArg = Number(arg("pid", "0")) || null;
const intervalSec = Number(arg("interval", "30"));
const hours = Number(arg("hours", "24"));
const minutes = Number(arg("minutes", "0"));
const samplesArg = Number(arg("samples", "0")) || null;
const maxHeat = Number(arg("max-heat", "60"));
const maxQueue = Number(arg("max-queue", "4"));
const maxDisconnects = Number(arg("max-disconnects", "10"));
const connectTimeoutSec = Number(arg("connect-timeout", "60"));
const noHeat = flag("no-heat");
const failFast = flag("fail-fast");
const python = process.env.ZEND_PYTHON || "python";
const stamp = new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19);
const outPath = path.resolve(
  arg("out", path.join(process.env.LOCALAPPDATA || ".", "zend", "logs", `soak-${stamp}.jsonl`)),
);

if (!(intervalSec > 0) || (samplesArg === null && !(hours > 0) && !(minutes > 0))) {
  console.error("soak: bad --interval/--hours/--minutes");
  process.exit(2);
}

const budgetMs = samplesArg === null ? (hours * 3600 + minutes * 60) * 1000 : Infinity;
fs.mkdirSync(path.dirname(outPath), { recursive: true });
const out = fs.createWriteStream(outPath, { flags: "a" });

function sessionPathFor(pid) {
  return path.join(process.env.LOCALAPPDATA || ".", "zend", "runtime", `session-${pid}.json`);
}

function newestSession(pid) {
  if (pid) {
    const file = sessionPathFor(pid);
    if (!fs.existsSync(file)) throw new Error(`no ${file}`);
    return JSON.parse(fs.readFileSync(file, "utf8"));
  }
  const dir = path.join(process.env.LOCALAPPDATA || ".", "zend", "runtime");
  const files = fs
    .readdirSync(dir)
    .filter((name) => name.startsWith("session-") && name.endsWith(".json"))
    .map((name) => path.join(dir, name))
    .sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs);
  if (files.length === 0) throw new Error(`no session file in ${dir}`);
  return JSON.parse(fs.readFileSync(files[0], "utf8"));
}

class Link {
  constructor() {
    this.socket = null;
    this.session = null;
    this.buffer = Buffer.alloc(0);
    this.pending = null;
    this.nextId = 1;
  }

  send(message) {
    const body = Buffer.from(JSON.stringify({ v: VERSION, ...message }), "utf8");
    const header = Buffer.alloc(4);
    header.writeUInt32LE(body.length, 0);
    this.socket.write(Buffer.concat([header, body]));
  }

  open(session) {
    this.session = session;
    const name = session.pipe.startsWith("\\\\.\\pipe\\")
      ? session.pipe
      : `\\\\.\\pipe\\${session.pipe}`;

    return new Promise((resolve, reject) => {
      const socket = net.connect(name);
      this.socket = socket;
      this.buffer = Buffer.alloc(0);
      const fail = (error) => {
        this.close();
        reject(error);
      };
      socket.setNoDelay(true);
      socket.once("error", fail);
      socket.once("connect", () => {
        socket.off("error", fail);
        socket.on("error", (error) => this.failPending(error));
        socket.on("close", () => this.failPending(new Error("pipe closed")));
        socket.on("data", (chunk) => this.onData(chunk));
        this.send({ op: "hello", id: 0, token: session.token, pid: process.pid });
        resolve();
      });
    });
  }

  close() {
    if (this.socket) {
      this.socket.removeAllListeners();
      this.socket.destroy();
      this.socket = null;
    }
    this.failPending(new Error("link closed"));
  }

  failPending(error) {
    if (this.pending) {
      const pending = this.pending;
      this.pending = null;
      pending.reject(error);
    }
  }

  onData(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (this.buffer.length >= 4) {
      const length = this.buffer.readUInt32LE(0);
      if (this.buffer.length < 4 + length) return;
      const message = JSON.parse(this.buffer.subarray(4, 4 + length).toString("utf8"));
      this.buffer = this.buffer.subarray(4 + length);
      if (message.op === "status.result" && this.pending) {
        const pending = this.pending;
        this.pending = null;
        pending.resolve(message);
      }
    }
  }

  status() {
    if (!this.socket) return Promise.reject(new Error("not connected"));
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        if (this.pending) {
          this.pending = null;
          reject(new Error("status timeout"));
        }
      }, REQUEST_TIMEOUT_MS);
      this.pending = {
        resolve: (message) => {
          clearTimeout(timer);
          resolve(message);
        },
        reject: (error) => {
          clearTimeout(timer);
          reject(error);
        },
      };
      this.send({ op: "status", id });
    });
  }
}

function readHeat(pid) {
  return new Promise((resolve) => {
    const child = spawn(
      python,
      [path.join(root, "tools", "detlog", "detlog.py"), "--pid", String(pid)],
      { windowsHide: true, stdio: ["ignore", "pipe", "ignore"] },
    );
    let text = "";
    const timer = setTimeout(() => child.kill(), 5000);
    child.stdout.on("data", (chunk) => {
      text += chunk.toString("utf8");
    });
    child.on("error", () => {
      clearTimeout(timer);
      resolve(null);
    });
    child.on("close", () => {
      clearTimeout(timer);
      const match = /heat (\d+)\/100\s+events (\d+)\s+drop (\d+)/.exec(text);
      resolve(match
        ? { heat: Number(match[1]), events: Number(match[2]), drop: Number(match[3]) }
        : null);
    });
  });
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const link = new Link();
let stopping = false;
let samples = 0;
let disconnects = 0;
let failures = 0;
let maxSeenHeat = 0;
let maxSeenQueue = 0;
let last = null;
let lastPid = null;
let lastEvents = 0;
let lastDrop = 0;
const started = Date.now();

for (const signal of ["SIGINT", "SIGTERM"]) {
  process.on(signal, () => {
    if (stopping) process.exit(130);
    stopping = true;
    console.log("soak: stopping after the current sample");
  });
}

function logFailure(reason, record) {
  failures += 1;
  console.error(`soak: FAIL ${reason} ${JSON.stringify(record)}`);
  if (failFast) {
    out.end();
    process.exit(1);
  }
}

async function connect() {
  const deadline = Date.now() + connectTimeoutSec * 1000;
  for (let attempt = 0; !stopping; attempt += 1) {
    try {
      const session = newestSession(pidArg);
      lastPid = session.pid;
      await link.open(session);
      console.log(`soak: attached pid ${session.pid} build ${session.build} via ${session.pipe}`);
      return true;
    } catch (error) {
      if (attempt === 0) console.error(`soak: connect failed: ${error.message}`);
      if (Date.now() >= deadline) {
        console.error(`soak: no live session after ${connectTimeoutSec}s`);
        return false;
      }
      await sleep(Math.min(5000, 500 * 2 ** Math.min(attempt, 4)));
    }
  }
  return false;
}

async function main() {
  console.log(`soak: out ${outPath} interval ${intervalSec}s budget ${
    samplesArg === null ? `${(budgetMs / 3600000).toFixed(2)}h` : `${samplesArg} samples`
  }`);

  if (!(await connect())) {
    out.end();
    return 2;
  }

  while (!stopping && (samplesArg === null || samples < samplesArg) && Date.now() - started < budgetMs) {
    await sleep(intervalSec * 1000);
    if (stopping) break;

    let status;
    try {
      status = await link.status();
    } catch (error) {
      disconnects += 1;
      console.error(`soak: disconnect ${disconnects}/${maxDisconnects}: ${error.message}`);
      if (disconnects > maxDisconnects) {
        logFailure("disconnects", { disconnects });
        break;
      }
      if (!(await connect())) {
        logFailure("session-lost", { disconnects });
        break;
      }
      continue;
    }

    samples += 1;
    const heat = noHeat || !lastPid ? null : await readHeat(lastPid);
    const record = {
      iso: new Date().toISOString(),
      ts: Date.now(),
      pid: lastPid,
      build: status.build ?? null,
      ready: status.ready === true,
      hooked: status.hooked === true,
      queue: Number(status.queue ?? -1),
      place_id: Number(status.place_id ?? 0),
      heat: heat ? heat.heat : null,
      events: heat ? heat.events : null,
      drop: heat ? heat.drop : null,
    };
    last = record;
    if (record.heat !== null) {
      maxSeenHeat = Math.max(maxSeenHeat, record.heat);
      lastEvents = record.events;
      lastDrop = record.drop;
    }
    maxSeenQueue = Math.max(maxSeenQueue, record.queue);
    out.write(`${JSON.stringify(record)}\n`);

    const time = record.iso.slice(11, 19);
    const heatText = record.heat === null ? "heat=-" : `heat=${record.heat}`;
    console.log(
      `[${time}] #${samples} ${heatText} queue=${record.queue} ready=${
        record.ready ? 1 : 0
      } hooked=${record.hooked ? 1 : 0}` +
        (record.events !== null ? ` events=${record.events} drop=${record.drop}` : ""),
    );

    if (!record.ready || !record.hooked) logFailure("not-ready", record);
    if (record.queue > maxQueue) logFailure("queue", record);
    if (record.heat !== null && record.heat > maxHeat) logFailure("heat", record);
  }

  link.close();
  out.end();

  const elapsed = (Date.now() - started) / 1000;
  console.log(
    `soak: ${failures === 0 ? "pass" : "FAIL"} samples=${samples} ` +
      `elapsed=${elapsed.toFixed(1)}s max-heat=${maxSeenHeat} max-queue=${maxSeenQueue} ` +
      `disconnects=${disconnects} events=${lastEvents} drop=${lastDrop}`,
  );
  console.log(`soak: ${outPath}`);
  return failures === 0 ? 0 : 1;
}

main().then(
  (code) => {
    process.exitCode = code;
  },
  (error) => {
    console.error(`soak: ${error.stack || error.message}`);
    process.exitCode = 2;
  },
);
