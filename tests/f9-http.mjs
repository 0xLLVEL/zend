#!/usr/bin/env node
// F9 live check: HTTP request / http_get / http_post.
//
//   node tests/f9-http.mjs [--pid <pid>]
//
// A local HTTP server is started by this script, so the check never needs
// external network access.

import fs from "node:fs";
import http from "node:http";
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

const server = http.createServer((req, res) => {
  const chunks = [];
  req.on("data", (chunk) => chunks.push(chunk));
  req.on("end", () => {
    const body = Buffer.concat(chunks).toString("utf8");
    const url = req.url ?? "/";

    if (url.startsWith("/delay")) {
      const ms = Number(new URL(url, "http://localhost").searchParams.get("ms") ?? "250");
      setTimeout(() => {
        res.writeHead(200, { "Content-Type": "text/plain" });
        res.end("delayed");
      }, Number.isFinite(ms) ? ms : 250);
      return;
    }

    if (url.startsWith("/status404")) {
      res.writeHead(404, { "Content-Type": "text/plain" });
      res.end("missing");
      return;
    }

    if (url.startsWith("/cookies")) {
      res.writeHead(200, {
        "Content-Type": "text/plain",
        "Set-Cookie": ["a=1; Path=/", "b=2; Path=/"],
      });
      res.end("cookies");
      return;
    }

    res.writeHead(200, {
      "Content-Type": "text/plain",
      "X-Served": "yes",
    });
    res.end(
      [
        req.method,
        url,
        body,
        req.headers["x-test"] ?? "",
        req.headers.cookie ?? "",
        req.headers["content-type"] ?? "",
      ].join("|"),
    );
  });
});

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
  console.error(`f9-check: ${err.message}`);
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

server.listen(0, "127.0.0.1", () => {
  const port = server.address().port;
  const base = `http://127.0.0.1:${port}`;

  socket.on("connect", async () => {
    try {
      console.log(`f9-check: ${pipe} · build ${session.build} · localhost:${port}`);
      send({ id: 0, op: "hello", token: session.token, pid: process.pid });

      const status = await request({ op: "status" });
      console.log(
        `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
      );

      await expect(
        "GET request with headers and cookies",
        `local r = request({ Url = "${base}/echo?q=1", Method = "get", Headers = { ["X-Test"] = "abc" }, Cookies = { sess = "xyz" } })
print(r.Success, r.StatusCode, r.StatusMessage)
print(r.Body)
print(type(r.Headers["Content-Type"]), r.Headers["X-Served"], r.Cookies.a)`,
        ["true\t200\tOK", "GET|/echo?q=1||abc|sess=xyz|", "string\tyes\tnil"],      );

      await expect(
        "POST body and content type reach the server",
        `local r = request({ Url = "${base}/echo", Method = "post", Body = "hello", Headers = { ["Content-Type"] = "text/plain", ["X-Test"] = "42" } })
print(r.Success, r.StatusCode, r.Body)`,
        ["true\t200\tPOST|/echo|hello|42||text/plain"],
      );

      await expect(
        "non-2xx keeps the body and reports Success=false",
        `local r = request({ Url = "${base}/status404" })
print(r.Success, r.StatusCode, r.StatusMessage, r.Body)`,
        ["false\t404\tNot Found\tmissing"],
      );

      await expect(
        "Set-Cookie headers become the Cookies table",
        `local r = request({ Url = "${base}/cookies" })
print(r.Cookies.a, r.Cookies.b, r.Body)`,
        ["1\t2\tcookies"],
      );

      await expect(
        "a slow request yields instead of blocking",
        `local start = tick()
local r = request({ Url = "${base}/delay?ms=250" })
print(r.Body, tick() - start >= 0.2)`,
        ["delayed\ttrue"],
      );

      await expect(
        "http_get and http_post wrappers",
        `print(http_get("${base}/echo"))
print(http_post("${base}/echo", "pbody"))
print(httpget == http_get, HttpPost == http_post, syn_request == request)
print(type(http.request), type(http.Request), http.http_request == request)`,
        [
          "GET|/echo||||",
          "POST|/echo|pbody|||application/json",
          "true\ttrue\ttrue",
          "function\tfunction\ttrue",
        ],
      );

      await expect(
        "invalid requests raise cleanly",
        `print((pcall(request, "not a table")))
print((pcall(request, { Url = "ftp://example.com" })))
print((pcall(request, { Url = "${base}/echo", Body = "x" })))
print((pcall(request, { Url = "${base}/echo", Headers = { ["Content-Length"] = "1" } })))
print((pcall(request, { Url = "http://auth.roblox.com/x" })))`,
        ["false", "false", "false", "false", "false"],
      );
    } catch (err) {
      failures += 1;
      console.log(`FAIL ${err.message}`);
    } finally {
      console.log(failures === 0 ? "f9: all checks passed" : `f9: ${failures} failure(s)`);
      socket.end();
      server.close();
      process.exit(failures === 0 ? 0 : 1);
    }
  });
});
