#!/usr/bin/env node
// Hermetic check for tools/soak: start the mock core, sample it three times,
// and verify the JSONL output and summary. Never touches Roblox.
//
//   node tests/soak-check.mjs

import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..");
const outDir = path.join(root, "build", "soak-check");
const outFile = path.join(outDir, "samples.jsonl");
fs.rmSync(outFile, { force: true });
fs.mkdirSync(outDir, { recursive: true });

const mock = spawn(process.execPath, [path.join(root, "tests", "mock-core.mjs")], {
  stdio: ["ignore", "pipe", "inherit"],
  windowsHide: true,
});
let mockLog = "";
mock.stdout.on("data", (chunk) => {
  mockLog += chunk.toString("utf8");
});

const sessionPath = path.join(
  process.env.LOCALAPPDATA,
  "zend",
  "runtime",
  `session-${mock.pid}.json`,
);

function waitForSession(timeoutMs = 6000) {
  const deadline = Date.now() + timeoutMs;
  return new Promise((resolve, reject) => {
    const timer = setInterval(() => {
      if (fs.existsSync(sessionPath)) {
        clearInterval(timer);
        resolve();
      } else if (Date.now() > deadline) {
        clearInterval(timer);
        reject(new Error(`mock session did not appear\n${mockLog}`));
      }
    }, 50);
  });
}

function runSoak() {
  return new Promise((resolve) => {
    const soak = spawn(
      process.execPath,
      [
        path.join(root, "tools", "soak", "soak.mjs"),
        "--pid", String(mock.pid),
        "--samples", "3",
        "--interval", "0.1",
        "--no-heat",
        "--out", outFile,
      ],
      { stdio: ["ignore", "pipe", "pipe"], windowsHide: true },
    );
    let stdout = "";
    let stderr = "";
    soak.stdout.on("data", (chunk) => {
      stdout += chunk.toString("utf8");
    });
    soak.stderr.on("data", (chunk) => {
      stderr += chunk.toString("utf8");
    });
    soak.on("close", (code) => resolve({ code, stdout, stderr }));
  });
}

function shutdownMock() {
  mock.kill("SIGTERM");
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      mock.kill();
      resolve();
    }, 3000);
    mock.on("exit", () => {
      clearTimeout(timer);
      resolve();
    });
  });
}

try {
  await waitForSession();
  const result = await runSoak();
  if (result.code !== 0) {
    throw new Error(`soak exited ${result.code}\n${result.stdout}\n${result.stderr}`);
  }
  if (!result.stdout.includes("soak: pass samples=3")) {
    throw new Error(`unexpected summary\n${result.stdout}`);
  }
  const lines = fs
    .readFileSync(outFile, "utf8")
    .split("\n")
    .filter(Boolean)
    .map((line) => JSON.parse(line));
  if (lines.length !== 3) {
    throw new Error(`expected 3 JSONL samples, got ${lines.length}`);
  }
  for (const line of lines) {
    if (line.hooked !== true || line.ready !== true || line.queue !== 0) {
      throw new Error(`bad sample ${JSON.stringify(line)}`);
    }
    if (line.heat !== null) throw new Error("heat should be null with --no-heat");
  }
  if (lines[2].build !== "4ea8e85481e34f46") {
    throw new Error(`unexpected build ${lines[2].build}`);
  }
  console.log(`soak-check: pass (${lines.length} samples, ${lines[0].pid})`);
} finally {
  await shutdownMock();
  fs.rmSync(outFile, { force: true });
}
