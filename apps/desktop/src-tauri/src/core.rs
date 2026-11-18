use std::collections::VecDeque;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use tauri::{AppHandle, Emitter};
use tokio::io::{AsyncReadExt, AsyncWriteExt, ReadHalf, WriteHalf};
use tokio::net::windows::named_pipe::{ClientOptions, NamedPipeClient};
use tokio::sync::mpsc;

const PROTOCOL_VERSION: u32 = 1;
const MAX_FRAME: u32 = 16 * 1024 * 1024;
const LOG_CAP: usize = 500;

#[derive(Clone, Serialize)]
pub struct Status {
    pub attached: bool,
    pub connecting: bool,
    pub build: String,
    pub place_id: i64,
    pub queue: u32,
    pub heat: u32,
    pub ping_ms: Option<u64>,
    pub pid: Option<u32>,
    pub error: Option<String>,
}

impl Default for Status {
    fn default() -> Self {
        Self {
            attached: false,
            connecting: false,
            build: String::new(),
            place_id: 0,
            queue: 0,
            heat: 0,
            ping_ms: None,
            pid: None,
            error: None,
        }
    }
}

#[derive(Clone, Serialize)]
pub struct LogEntry {
    pub level: String,
    pub msg: String,
    pub ts: u64,
}

#[derive(Serialize)]
pub struct Snapshot {
    pub status: Status,
    pub logs: Vec<LogEntry>,
}

#[derive(Clone, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum CoreEvent {
    Status { status: Status },
    Log { level: String, msg: String },
    Result {
        id: u64,
        ok: bool,
        output: String,
        error: Option<String>,
        ms: u64,
    },
    Output {
        id: u64,
        kind: String,
        text: String,
    },
    Toast {
        title: String,
        content: String,
        kind: String,
    },
}

impl CoreEvent {
    fn emit(&self, app: &AppHandle) {
        let started = Instant::now();
        let _ = app.emit("core-event", self);
        let ms = started.elapsed().as_millis();
        if ms >= 20 {
            app_log(format!("emit {} blocked {ms} ms", self.kind()));
        }
    }

    fn kind(&self) -> &'static str {
        match self {
            CoreEvent::Status { .. } => "status",
            CoreEvent::Log { .. } => "log",
            CoreEvent::Result { .. } => "result",
            CoreEvent::Output { .. } => "output",
            CoreEvent::Toast { .. } => "toast",
        }
    }
}

// Diagnostic only: reader/emit timings are invisible in the UI because a
// stalled emit also stalls the log line it would produce.
fn app_log(msg: impl AsRef<str>) {
    use std::io::Write;

    let Some(dir) = std::env::var_os("LOCALAPPDATA") else {
        return;
    };
    let path = PathBuf::from(dir).join("zend").join("logs").join("app.log");
    if let Ok(mut file) = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
    {
        let _ = writeln!(file, "{} {}", now_ms(), msg.as_ref());
    }
}

struct Conn {
    tx: mpsc::UnboundedSender<Vec<u8>>,
    reader: tokio::task::JoinHandle<()>,
    writer: tokio::task::JoinHandle<()>,
}

pub struct Runtime {
    status: Mutex<Status>,
    logs: Mutex<VecDeque<LogEntry>>,
    conn: Mutex<Option<Conn>>,
    next_id: AtomicU64,
    generation: AtomicU64,
    pending_ping: Mutex<Option<(u64, Instant)>>,
    pipe_filter: Mutex<Option<String>>,
}

impl Default for Runtime {
    fn default() -> Self {
        Self {
            status: Mutex::new(Status::default()),
            logs: Mutex::new(VecDeque::new()),
            conn: Mutex::new(None),
            next_id: AtomicU64::new(0),
            generation: AtomicU64::new(0),
            pending_ping: Mutex::new(None),
            pipe_filter: Mutex::new(None),
        }
    }
}

#[derive(Clone, Deserialize)]
struct SessionFile {
    v: u32,
    pid: u32,
    pipe: String,
    token: String,
    build: String,
    ts: u64,
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

fn session_dir() -> Option<PathBuf> {
    let base = std::env::var_os("LOCALAPPDATA")?;
    Some(PathBuf::from(base).join("zend").join("runtime"))
}

fn normalize_pipe(name: &str) -> &str {
    name.trim().trim_start_matches(r"\\.\pipe\")
}

#[cfg(windows)]
fn pid_alive(pid: u32) -> bool {
    use std::ffi::c_void;

    unsafe extern "system" {
        fn OpenProcess(access: u32, inherit: i32, pid: u32) -> *mut c_void;
        fn GetExitCodeProcess(handle: *mut c_void, code: *mut u32) -> i32;
        fn CloseHandle(handle: *mut c_void) -> i32;
    }

    const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;
    const STILL_ACTIVE: u32 = 259;

    unsafe {
        let handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if handle.is_null() {
            return false;
        }
        let mut code = 0u32;
        let ok = GetExitCodeProcess(handle, &mut code) != 0;
        CloseHandle(handle);
        ok && code == STILL_ACTIVE
    }
}

fn find_session(pipe_filter: Option<&str>) -> Option<(PathBuf, SessionFile)> {
    let dir = session_dir()?;
    let entries = std::fs::read_dir(dir).ok()?;
    let filter = pipe_filter.map(normalize_pipe).filter(|name| !name.is_empty());
    let mut best: Option<(PathBuf, SessionFile)> = None;

    for entry in entries.flatten() {
        let path = entry.path();
        let name = match path.file_name().and_then(|n| n.to_str()) {
            Some(name) => name,
            None => continue,
        };
        if !name.starts_with("session-") || !name.ends_with(".json") {
            continue;
        }

        let session: SessionFile = match std::fs::read_to_string(&path)
            .ok()
            .and_then(|raw| serde_json::from_str(&raw).ok())
        {
            Some(session) => session,
            None => continue,
        };

        if session.v != PROTOCOL_VERSION {
            continue;
        }
        // Sessions are written once when the core loads. A live client can
        // outlive any freshness window, and a crashed one can leave a fresh
        // (or clock-skewed) timestamp behind, so trust the PID instead.
        if !pid_alive(session.pid) {
            continue;
        }
        if filter.is_some_and(|want| normalize_pipe(&session.pipe) != want) {
            continue;
        }

        let newer = best
            .as_ref()
            .map(|(_, current)| session.ts >= current.ts)
            .unwrap_or(true);
        if newer {
            best = Some((path, session));
        }
    }

    best
}

fn pipe_path(name: &str) -> String {
    if name.starts_with(r"\\.\pipe\") {
        name.to_string()
    } else {
        format!(r"\\.\pipe\{name}")
    }
}

fn set_status(app: &AppHandle, rt: &Runtime, change: impl FnOnce(&mut Status)) {
    let snapshot = {
        let mut status = rt.status.lock().unwrap();
        change(&mut status);
        status.clone()
    };
    CoreEvent::Status { status: snapshot }.emit(app);
}

fn push_log(app: &AppHandle, rt: &Runtime, level: &str, msg: impl Into<String>) {
    let msg = msg.into();
    {
        let mut logs = rt.logs.lock().unwrap();
        if logs.len() >= LOG_CAP {
            logs.pop_front();
        }
        logs.push_back(LogEntry {
            level: level.to_string(),
            msg: msg.clone(),
            ts: now_ms(),
        });
    }
    CoreEvent::Log {
        level: level.to_string(),
        msg,
    }
    .emit(app);
}

fn send_value(rt: &Runtime, value: Value) -> Result<(), String> {
    let payload = serde_json::to_vec(&value).map_err(|err| err.to_string())?;
    if payload.len() > MAX_FRAME as usize {
        return Err("frame exceeds 16 MiB".into());
    }

    let guard = rt.conn.lock().unwrap();
    let conn = guard.as_ref().ok_or("no client attached")?;
    conn.tx.send(payload).map_err(|_| "pipe closed".to_string())
}

fn handle_message(app: &AppHandle, rt: &Runtime, value: &Value) {
    let op = value.get("op").and_then(Value::as_str).unwrap_or("");

    match op {
        "log" => {
            let level = value
                .get("level")
                .and_then(Value::as_str)
                .filter(|level| matches!(*level, "info" | "warn" | "error" | "debug"))
                .unwrap_or("info");
            let msg = value.get("msg").and_then(Value::as_str).unwrap_or("");
            push_log(app, rt, level, msg);
        }
        "output" => {
            let id = value.get("id").and_then(Value::as_u64).unwrap_or(0);
            let kind = value
                .get("kind")
                .and_then(Value::as_str)
                .filter(|kind| matches!(*kind, "print" | "warn"))
                .unwrap_or("print")
                .to_string();
            let text = value
                .get("text")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string();
            CoreEvent::Output { id, kind, text }.emit(app);
        }
        "result" => {
            let id = value.get("id").and_then(Value::as_u64).unwrap_or(0);
            let ok = value.get("ok").and_then(Value::as_bool).unwrap_or(false);
            let output = value
                .get("output")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string();
            let error = value
                .get("error")
                .and_then(Value::as_str)
                .map(str::to_owned);
            let ms = value.get("ms").and_then(Value::as_u64).unwrap_or(0);

            CoreEvent::Result {
                id,
                ok,
                output,
                error,
                ms,
            }
            .emit(app);

            set_status(app, rt, |status| {
                status.queue = status.queue.saturating_sub(1);
            });
        }
        "status.result" => {
            let build = value
                .get("build")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string();
            let place_id = value.get("place_id").and_then(Value::as_i64).unwrap_or(0);
            let queue = value.get("queue").and_then(Value::as_u64).unwrap_or(0) as u32;
            let heat = value.get("heat").and_then(Value::as_u64).unwrap_or(0) as u32;

            set_status(app, rt, |status| {
                if !build.is_empty() {
                    status.build = build;
                }
                status.place_id = place_id;
                status.queue = queue;
                status.heat = heat;
            });
        }
        "pong" => {
            let id = value.get("id").and_then(Value::as_u64);
            let mut pending = rt.pending_ping.lock().unwrap();
            if let Some((expected, at)) = *pending {
                if id.is_none() || id == Some(expected) {
                    *pending = None;
                    drop(pending);
                    let ms = at.elapsed().as_millis() as u64;
                    set_status(app, rt, |status| status.ping_ms = Some(ms));
                }
            }
        }
        "toast" => {
            let title = value
                .get("title")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string();
            let content = value
                .get("content")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string();
            let kind = value
                .get("kind")
                .and_then(Value::as_str)
                .unwrap_or("info")
                .to_string();
            CoreEvent::Toast {
                title,
                content,
                kind,
            }
            .emit(app);
        }
        _ => {}
    }
}

async fn writer_task(
    mut write: WriteHalf<NamedPipeClient>,
    mut rx: mpsc::UnboundedReceiver<Vec<u8>>,
) {
    while let Some(payload) = rx.recv().await {
        let mut framed = Vec::with_capacity(4 + payload.len());
        framed.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        framed.extend_from_slice(&payload);

        let started = Instant::now();
        if write.write_all(&framed).await.is_err() {
            break;
        }
        if write.flush().await.is_err() {
            break;
        }
        let ms = started.elapsed().as_millis();
        if ms >= 20 {
            app_log(format!("write {} bytes took {ms} ms", framed.len()));
        }
    }
}

async fn reader_task(
    app: AppHandle,
    rt: Arc<Runtime>,
    generation: u64,
    mut read: ReadHalf<NamedPipeClient>,
) {
    let mut reason = String::from("connection closed");

    loop {
        let mut header = [0u8; 4];
        if let Err(err) = read.read_exact(&mut header).await {
            if err.kind() != std::io::ErrorKind::UnexpectedEof {
                reason = format!("read failed: {err}");
            }
            break;
        }

        let len = u32::from_le_bytes(header);
        if len > MAX_FRAME {
            reason = "frame exceeds 16 MiB".into();
            break;
        }

        let mut body = vec![0u8; len as usize];
        if let Err(err) = read.read_exact(&mut body).await {
            reason = format!("read failed: {err}");
            break;
        }

        let value: Value = match serde_json::from_slice(&body) {
            Ok(value) => value,
            Err(err) => {
                push_log(&app, &rt, "warn", format!("protocol error: {err}"));
                continue;
            }
        };

        let op = value
            .get("op")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string();
        let started = Instant::now();
        handle_message(&app, &rt, &value);
        let ms = started.elapsed().as_millis();
        if ms >= 20 {
            app_log(format!("handle {op} took {ms} ms"));
        }
    }

    if rt.generation.load(Ordering::SeqCst) != generation {
        return;
    }

    let taken = {
        let mut guard = rt.conn.lock().unwrap();
        guard.take().is_some()
    };

    if taken {
        set_status(&app, &rt, |status| {
            status.attached = false;
            status.connecting = false;
            status.pid = None;
            status.ping_ms = None;
        });
        push_log(&app, &rt, "warn", reason);
    }
}

async fn heartbeat(
    rt: Arc<Runtime>,
    tx: mpsc::UnboundedSender<Vec<u8>>,
    generation: u64,
) {
    loop {
        tokio::time::sleep(Duration::from_secs(5)).await;

        if rt.generation.load(Ordering::SeqCst) != generation {
            break;
        }

        let id = rt.next_id.fetch_add(1, Ordering::Relaxed) + 1;
        *rt.pending_ping.lock().unwrap() = Some((id, Instant::now()));

        let payload = match serde_json::to_vec(&json!({
            "v": PROTOCOL_VERSION,
            "id": id,
            "op": "ping"
        })) {
            Ok(payload) => payload,
            Err(_) => break,
        };

        if tx.send(payload).is_err() {
            break;
        }
    }
}

async fn try_connect(app: &AppHandle, rt: &Arc<Runtime>, session: SessionFile) -> Result<(), String> {
    if rt.conn.lock().unwrap().is_some() {
        return Ok(());
    }

    set_status(app, rt, |status| {
        status.connecting = true;
        status.error = None;
    });

    let path = pipe_path(&session.pipe);
    let client = ClientOptions::new()
        .open(&path)
        .map_err(|err| format!("open {path}: {err}"))?;

    let (read, write) = tokio::io::split(client);
    let (tx, rx) = mpsc::unbounded_channel();
    let generation = rt.generation.fetch_add(1, Ordering::SeqCst) + 1;

    let writer = tokio::spawn(writer_task(write, rx));
    let reader = tokio::spawn(reader_task(
        app.clone(),
        rt.clone(),
        generation,
        read,
    ));

    {
        let mut guard = rt.conn.lock().unwrap();
        *guard = Some(Conn {
            tx: tx.clone(),
            reader,
            writer,
        });
    }

    let hello = json!({
        "v": PROTOCOL_VERSION,
        "id": 0,
        "op": "hello",
        "token": session.token,
        "pid": std::process::id()
    });
    let payload = serde_json::to_vec(&hello).map_err(|err| err.to_string())?;
    if tx.send(payload).is_err() {
        let mut guard = rt.conn.lock().unwrap();
        guard.take();
        return Err("pipe closed during handshake".into());
    }

    // Confirm build/place/queue from the core itself, not only the session file.
    let status_id = rt.next_id.fetch_add(1, Ordering::Relaxed) + 1;
    if let Ok(payload) = serde_json::to_vec(&json!({
        "v": PROTOCOL_VERSION,
        "id": status_id,
        "op": "status"
    })) {
        let _ = tx.send(payload);
    }

    set_status(app, rt, |status| {
        status.attached = true;
        status.connecting = false;
        status.build = session.build.clone();
        status.pid = Some(session.pid);
        status.queue = 0;
        status.heat = 0;
        status.ping_ms = None;
        status.error = None;
    });

    push_log(app, rt, "info", format!("attached to pid {}", session.pid));
    tokio::spawn(heartbeat(rt.clone(), tx, generation));

    Ok(())
}

async fn supervisor(app: AppHandle, rt: Arc<Runtime>) {
    let mut last_error: Option<String> = None;
    tokio::time::sleep(Duration::from_millis(400)).await;

    loop {
        let busy = rt.conn.lock().unwrap().is_some();

        if !busy {
            let filter = rt.pipe_filter.lock().unwrap().clone();
            match find_session(filter.as_deref()) {
                Some((_path, session)) => match try_connect(&app, &rt, session).await {
                    Ok(()) => last_error = None,
                    Err(err) => {
                        if last_error.as_deref() != Some(err.as_str()) {
                            push_log(&app, &rt, "warn", format!("attach failed: {err}"));
                            last_error = Some(err.clone());
                        }
                        set_status(&app, &rt, |status| {
                            status.connecting = false;
                            status.error = Some(err);
                        });
                    }
                },
                None => {
                    if last_error.is_some() {
                        set_status(&app, &rt, |status| {
                            status.connecting = false;
                            status.error = None;
                        });
                        last_error = None;
                    }
                }
            }
        }

        tokio::time::sleep(Duration::from_millis(1000)).await;
    }
}

pub fn start(app: AppHandle, rt: Arc<Runtime>) {
    let _ = std::thread::Builder::new()
        .name("zend-link".into())
        .spawn(move || {
            let runtime = match tokio::runtime::Builder::new_multi_thread()
                .enable_all()
                .thread_name("zend-pipe")
                .build()
            {
                Ok(runtime) => runtime,
                Err(err) => {
                    eprintln!("zend: failed to start pipe runtime: {err}");
                    return;
                }
            };
            runtime.block_on(supervisor(app, rt));
        });
}

pub fn snapshot(rt: &Runtime) -> Snapshot {
    Snapshot {
        status: rt.status.lock().unwrap().clone(),
        logs: rt.logs.lock().unwrap().iter().cloned().collect(),
    }
}

pub fn exec_script(
    app: &AppHandle,
    rt: &Runtime,
    source: String,
    chunk: String,
) -> Result<u64, String> {
    if source.len() > MAX_FRAME as usize {
        return Err("script exceeds 16 MiB".into());
    }

    let id = rt.next_id.fetch_add(1, Ordering::Relaxed) + 1;
    send_value(
        rt,
        json!({
            "v": PROTOCOL_VERSION,
            "id": id,
            "op": "exec",
            "source": source,
            "chunk": chunk,
            // Chunks may yield; give the core room to report a real result.
            "timeout_ms": 30000
        }),
    )?;

    set_status(app, rt, |status| {
        status.queue = status.queue.saturating_add(1);
    });

    Ok(id)
}

pub fn detach(app: &AppHandle, rt: &Runtime) -> Result<(), String> {
    let id = rt.next_id.fetch_add(1, Ordering::Relaxed) + 1;
    let _ = send_value(
        rt,
        json!({
            "v": PROTOCOL_VERSION,
            "id": id,
            "op": "detach"
        }),
    );

    rt.generation.fetch_add(1, Ordering::SeqCst);

    let mut guard = rt.conn.lock().unwrap();
    if let Some(conn) = guard.take() {
        conn.reader.abort();
        conn.writer.abort();
    }
    drop(guard);

    *rt.pending_ping.lock().unwrap() = None;
    set_status(app, rt, |status| {
        status.attached = false;
        status.connecting = false;
        status.pid = None;
        status.ping_ms = None;
        status.error = None;
    });
    push_log(app, rt, "info", "detached");

    Ok(())
}

pub fn set_pipe(rt: &Runtime, name: &str) {
    let trimmed = name.trim();
    let mut filter = rt.pipe_filter.lock().unwrap();
    *filter = if trimmed.is_empty() {
        None
    } else {
        Some(trimmed.to_string())
    };
}

pub fn connect_now(app: &AppHandle, rt: &Runtime) -> Result<(), String> {
    if rt.conn.lock().unwrap().is_some() {
        return Ok(());
    }

    let filter = rt.pipe_filter.lock().unwrap().clone();
    find_session(filter.as_deref()).ok_or_else(|| {
        "no matching session file in %LOCALAPPDATA%\\zend\\runtime".to_string()
    })?;

    set_status(app, rt, |status| {
        status.connecting = true;
        status.error = None;
    });

    Ok(())
}


