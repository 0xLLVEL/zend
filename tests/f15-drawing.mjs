#!/usr/bin/env node
// F15 live check: Drawing object model and the Present overlay.
//
//   node tests/f15-drawing.mjs [--pid <pid>]
//
// The last section draws a scene and holds it for 8 seconds. Watch the Roblox
// window during that window; the script then removes everything.

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
  console.error(`f15-check: ${err.message}`);
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
    console.log(`f15-check: ${pipe} · build ${session.build}`);
    send({ id: 0, op: "hello", token: session.token, pid: process.pid });

    const status = await request({ op: "status" });
    console.log(
      `status: place=${status.place_id} queue=${status.queue} hooked=${status.hooked} ready=${status.ready}`,
    );

    await expect(
      "Drawing and helpers are registered",
      `print(type(Drawing), type(Drawing.new), type(Drawing.Fonts))
print(type(cleardrawcache), type(isrenderobj), type(getrenderproperty), type(setrenderproperty))`,
      ["table\tfunction\ttable", "function\tfunction\tfunction\tfunction"],
    );

    await expect(
      "square properties round-trip",
      `local square = Drawing.new("Square")
square.Position = Vector2.new(100, 120)
square.Size = Vector2.new(240, 160)
square.Color = Color3.fromRGB(255, 0, 0)
square.Filled = true
square.Visible = true
square.ZIndex = 7
square.Thickness = 3
print(square.Class, square.ClassName, square.Visible, square.Filled,
      square.ZIndex, square.Thickness, square.Transparency)
print(square.Position.X, square.Position.Y, square.Size.X, square.Size.Y)
print(square.Color.R, square.Color.G, square.Color.B)
print(isrenderobj(square), getrenderproperty(square, "Class"))
setrenderproperty(square, "Filled", false)
print(square.Filled)
square:Remove()`,
      [
        "Square\tSquare\ttrue\ttrue\t7\t3\t1",
        "100\t120\t240\t160",
        "1\t0\t0",
        "true\tSquare",
        "false",
      ],
    );

    await expect(
      "text reports bounds and follows font/outline properties",
      `local text = Drawing.new("Text")
text.Text = "zend drawing ok"
text.Size = 24
text.Position = Vector2.new(200, 200)
text.Center = true
text.Outline = true
text.OutlineColor = Color3.fromRGB(0, 0, 0)
text.Font = 2
text.Visible = true
local bounds = text.TextBounds
print(text.Class, text.Text, text.Size, text.Font, text.Center, text.Outline)
print(bounds.X > 0, bounds.Y > 0)
text:Destroy()`,
      [
        "Text\tzend drawing ok\t24\t2\ttrue\ttrue",
        "true\ttrue",
      ],
    );

    await expect(
      "circle, triangle, quad, and line keep their own fields",
      `local circle = Drawing.new("Circle")
circle.Position = Vector2.new(300, 300)
circle.Radius = 50
circle.NumSides = 48
circle.Filled = true
local triangle = Drawing.new("Triangle")
triangle.PointA = Vector2.new(0, 0)
triangle.PointB = Vector2.new(40, 0)
triangle.PointC = Vector2.new(20, 30)
local quad = Drawing.new("Quad")
quad.PointA = Vector2.new(0, 0)
quad.PointB = Vector2.new(10, 0)
quad.PointC = Vector2.new(10, 10)
quad.PointD = Vector2.new(0, 10)
local line = Drawing.new("Line")
line.From = Vector2.new(1, 2)
line.To = Vector2.new(3, 4)
print(circle.Radius, circle.NumSides, circle.Filled)
print(triangle.PointC.X, triangle.PointC.Y, quad.PointD.X, quad.PointD.Y)
print(line.From.X, line.From.Y, line.To.X, line.To.Y)
for _, object in {circle, triangle, quad, line} do object:Remove() end`,
      [
        "50\t48\ttrue",
        "20\t30\t0\t10",
        "1\t2\t3\t4",
      ],
    );

    await expect(
      "destroyed objects stay inert and tostring is honest",
      `local square = Drawing.new("Square")
square.Visible = true
local before = getrenderproperty(square, "__OBJECT_EXISTS")
square:Remove()
local after = getrenderproperty(square, "__OBJECT_EXISTS")
print(before, after, isrenderobj(square), tostring(square))
square.Visible = true
print(getrenderproperty(square, "Visible"))`,
      ["true\tfalse\ttrue\tDrawing<destroyed>", "nil"],
    );

    await expect(
      "invalid classes fail cleanly and cache clear is callable",
      `local ok = pcall(function() return Drawing.new("Image") end)
print(ok)
cleardrawcache()
print(type(Drawing.new("Line")))`,
      ["false", "userdata"],
    );

    const result = await exec(
      `local camera = workspace.CurrentCamera
local viewport = camera and camera.ViewportSize or Vector2.new(1280, 720)
local cx, cy = viewport.X * 0.5, viewport.Y * 0.5
local objects = {}
local function keep(object)
    objects[#objects + 1] = object
    object.Visible = true
    return object
end
local panel = keep(Drawing.new("Square"))
panel.Position = Vector2.new(cx - 180, cy - 120)
panel.Size = Vector2.new(360, 240)
panel.Color = Color3.fromRGB(20, 20, 28)
panel.Transparency = 0.85
panel.Filled = true
panel.ZIndex = 1
local border = keep(Drawing.new("Square"))
border.Position = panel.Position
border.Size = panel.Size
border.Color = Color3.fromRGB(90, 200, 255)
border.Thickness = 2
border.Filled = false
border.ZIndex = 2
local circle = keep(Drawing.new("Circle"))
circle.Position = Vector2.new(cx - 110, cy)
circle.Radius = 44
circle.NumSides = 64
circle.Color = Color3.fromRGB(80, 255, 140)
circle.Thickness = 3
circle.ZIndex = 3
local triangle = keep(Drawing.new("Triangle"))
triangle.PointA = Vector2.new(cx + 40, cy - 60)
triangle.PointB = Vector2.new(cx + 140, cy + 60)
triangle.PointC = Vector2.new(cx - 60, cy + 60)
triangle.Color = Color3.fromRGB(255, 210, 70)
triangle.Filled = true
triangle.Transparency = 0.75
triangle.ZIndex = 3
local line = keep(Drawing.new("Line"))
line.From = Vector2.new(cx - 180, cy - 140)
line.To = Vector2.new(cx + 180, cy + 120)
line.Color = Color3.fromRGB(255, 70, 120)
line.Thickness = 2
line.ZIndex = 4
local text = keep(Drawing.new("Text"))
text.Text = "zend drawing ok"
text.Size = 26
text.Position = Vector2.new(cx, cy - 170)
text.Center = true
text.Outline = true
text.OutlineColor = Color3.fromRGB(0, 0, 0)
text.Color = Color3.fromRGB(255, 255, 255)
text.ZIndex = 5
print("visual scene up for 8 seconds")
task.wait(8)
cleardrawcache()
print("visual scene cleared")`,
    );
    const actual = lines(result);
    check(
      "visual scene renders without a fault",
      result.ok &&
        actual[0] === "visual scene up for 8 seconds" &&
        actual[1] === "visual scene cleared",
      { ok: result.ok, output: result.output, error: result.error },
    );
  } catch (err) {
    failures += 1;
    console.log(`FAIL ${err.message}`);
  } finally {
    console.log(failures === 0 ? "f15: all checks passed" : `f15: ${failures} failure(s)`);
    socket.end();
    process.exit(failures === 0 ? 0 : 1);
  }
});
