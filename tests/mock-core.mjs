#!/usr/bin/env node
// Mock ZEND core. Speaks protocol v1 over a named pipe and writes the session
// file the desktop app watches. Used by F1 tests; it never touches Roblox.
//
//   node tests/mock-core.mjs [--build 4ea8e85481e34f46] [--token abc]
//
// Ctrl+C removes the session file.

import crypto from "node:crypto";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";

const VERSION = 1;
const MAX_FRAME = 16 * 1024 * 1024;

function arg(name, fallback) {
  const index = process.argv.indexOf(`--${name}`);
  return index > 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
}

const build = arg("build", "4ea8e85481e34f46");
const token = arg("token", crypto.randomBytes(16).toString("base64"));
const nonce = crypto.randomBytes(6).toString("hex");
const pipeName = `zend-${nonce}`;
const pipePath = `\\\\.\\pipe\\${pipeName}`;

const runtimeDir = path.join(process.env.LOCALAPPDATA, "zend", "runtime");
const sessionPath = path.join(runtimeDir, `session-${process.pid}.json`);

fs.mkdirSync(runtimeDir, { recursive: true });

const session = {
  v: VERSION,
  pid: process.pid,
  pipe: pipeName,
  token,
  build,
  ts: Math.floor(Date.now() / 1000),
};

const tmp = `${sessionPath}.tmp`;
fs.writeFileSync(tmp, JSON.stringify(session));
fs.renameSync(tmp, sessionPath);

function cleanup() {
  try {
    fs.rmSync(sessionPath, { force: true });
  } catch {
    // best effort
  }
}

function send(socket, message) {
  const payload = Buffer.from(JSON.stringify({ v: VERSION, ...message }), "utf8");
  const header = Buffer.alloc(4);
  header.writeUInt32LE(payload.length, 0);
  socket.write(Buffer.concat([header, payload]));
}

function fakeOutput(source, chunk) {
  const prints = [...source.matchAll(/print\s*\(\s*((?:"[^"]*"|'[^']*'|[^)]*))\s*\)/g)]
    .map((match) =>
      match[1]
        .split(",")
        .map((part) => part.trim().replace(/^["']|["']$/g, ""))
        .join(" "),
    )
    .filter(Boolean);

  if (prints.length > 0) return prints;

  const lines = source.split(/\r?\n/).filter((line) => line.trim()).length;
  return [`[mock] ${chunk} executed (${lines} logical lines)`];
}

function fakeError(source) {
  const match = source.match(/error\s*\(\s*["']([^"']*)["']/);
  return match ? match[1] : null;
}

let helloToken = null;

const server = net.createServer((socket) => {
  let buffer = Buffer.alloc(0);

  socket.on("data", (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);

    while (buffer.length >= 4) {
      const length = buffer.readUInt32LE(0);
      if (length > MAX_FRAME) {
        socket.destroy();
        return;
      }
      if (buffer.length < 4 + length) return;

      const body = buffer.subarray(4, 4 + length);
      buffer = buffer.subarray(4 + length);

      let message;
      try {
        message = JSON.parse(body.toString("utf8"));
      } catch {
        continue;
      }

      handle(socket, message);
    }
  });

  socket.on("error", () => {});
});

function handle(socket, message) {
  const op = message.op;

  if (op === "hello") {
    helloToken = message.token;
    if (helloToken !== token) {
      send(socket, { op: "log", level: "error", msg: "mock core: bad token", ts: Date.now() });
      socket.end();
      return;
    }

    console.log(`[mock] hello from pid ${message.pid}`);
    send(socket, {
      op: "log",
      level: "info",
      msg: `mock core attached · build ${build}`,
      ts: Date.now(),
    });
    send(socket, {
      op: "toast",
      title: "Mock core attached",
      content: "No Roblox is running; exec is simulated.",
      kind: "info",
    });
    send(socket, {
      op: "status.result",
      id: message.id ?? 1,
      build,
      attached: true,
      place_id: 0,
      queue: 0,
      heat: 0,
      ready: true,
      hooked: true,
    });
    return;
  }

  if (op === "exec") {
    if (helloToken !== token) {
      socket.end();
      return;
    }

    send(socket, {
      op: "status.result",
      id: 0,
      build,
      attached: true,
      place_id: 0,
      queue: 1,
      heat: 0,
      ready: true,
      hooked: true,
    });

    const started = Date.now();
    const failure = fakeError(message.source ?? "");

    setTimeout(() => {
      const ms = Date.now() - started;
      if (failure) {
        send(socket, {
          op: "result",
          id: message.id,
          ok: false,
          output: "",
          error: `[string "${message.chunk}"]:1: ${failure}`,
          ms,
        });
        return;
      }

      const output = fakeOutput(message.source ?? "", message.chunk ?? "chunk");
      send(socket, {
        op: "log",
        level: "info",
        msg: `mock ran ${message.chunk} in ${ms} ms`,
        ts: Date.now(),
      });
      send(socket, {
        op: "result",
        id: message.id,
        ok: true,
        output: output.join("\n"),
        error: null,
        ms,
      });
    }, 60 + Math.random() * 120);
    return;
  }

  if (op === "status") {
    send(socket, {
      op: "status.result",
      id: message.id,
      build,
      attached: true,
      place_id: 0,
      queue: 0,
      heat: 0,
      ready: true,
      hooked: true,
    });
    return;
  }

  if (op === "ping") {
    send(socket, { op: "pong", id: message.id });
    return;
  }

  if (op === "detach") {
    socket.end();
    shutdown(0);
  }
}

function shutdown(code) {
  cleanup();
  server.close(() => process.exit(code));
  setTimeout(() => process.exit(code), 300).unref();
}

process.on("SIGINT", () => shutdown(0));
process.on("SIGTERM", () => shutdown(0));
process.on("exit", cleanup);

server.listen(pipePath, () => {
  console.log(`[mock] listening on ${pipePath}`);
  console.log(`[mock] session ${sessionPath}`);
  console.log(`[mock] token ${token}`);
  console.log(`[mock] pid ${process.pid} · build ${build} · ${os.platform()}`);
});
