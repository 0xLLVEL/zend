import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import type { UnlistenFn } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { TitleBar } from "./components/TitleBar";
import { Rail, type View } from "./components/Rail";
import { ScriptList } from "./components/ScriptList";
import { Workbench } from "./components/Workbench";
import {
  ConsolePanel,
  type LogFilters,
  type LogLevel,
  type LogLine,
  type RunTag,
} from "./components/ConsolePanel";
import { AutoexecPanel } from "./components/AutoexecPanel";
import { SettingsPanel } from "./components/SettingsPanel";
import { HubPanel } from "./components/HubPanel";
import { StatusBar } from "./components/StatusBar";
import { loadDocs, makeScript, saveDocs, type ScriptDoc } from "./data/scripts";
import { loadSession, loadSettings, saveSession, saveSettings, type Settings } from "./data/settings";
import {
  OFFLINE,
  coreConnect,
  coreDetach,
  coreExec,
  coreSetPipe,
  coreSnapshot,
  listenCore,
  type CoreStatus,
} from "./lib/core";
import "./styles/app.css";

const LOG_CAP = 500;
const VIEWS: View[] = ["scripts", "hub", "autoexec", "console", "settings"];

let logSeq = 0;

function stamp(ms?: number): string {
  const date = ms === undefined ? new Date() : new Date(ms);
  return date.toLocaleTimeString("en-GB", { hour12: false });
}

function logLine(level: LogLevel, msg: string, ts?: number, run?: RunTag): LogLine {
  logSeq += 1;
  return { id: logSeq, ts: stamp(ts), level, msg, run };
}

function initialLogs(): LogLine[] {
  return [
    logLine("info", "zend desktop 0.1.0"),
    logLine("info", "protocol v1 · named pipe"),
    logLine("warn", "no client session — start Roblox through the ZEND loader"),
  ];
}

function errorText(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

interface AppInfo {
  version: string;
  client_build?: string;
  client_version?: string;
}

interface Toast {
  id: number;
  title: string;
  body: string;
}

export default function App() {
  const [settings, setSettings] = useState<Settings>(loadSettings);
  const [docs, setDocs] = useState<ScriptDoc[]>(loadDocs);
  const [storedSession] = useState(loadSession);
  const [view, setView] = useState<View>(() =>
    VIEWS.includes(storedSession.view as View) ? (storedSession.view as View) : "scripts",
  );
  const [activeId, setActiveId] = useState<string>(() => {
    const hit = docs.find((doc) => doc.id === storedSession.activeId);
    return hit?.id ?? docs[0]?.id ?? "";
  });
  const [logs, setLogs] = useState<LogLine[]>(initialLogs);
  const [status, setStatus] = useState<CoreStatus>(OFFLINE);
  const [version, setVersion] = useState("0.1.0");
  const [clientBuild, setClientBuild] = useState("");
  const [clientVersion, setClientVersion] = useState("");
  const [toast, setToast] = useState<Toast | null>(null);
  const [autoexecAt, setAutoexecAt] = useState<number | null>(null);

  const activeDoc = useMemo(
    () => docs.find((doc) => doc.id === activeId) ?? docs[0],
    [docs, activeId],
  );

  useEffect(() => {
    if (activeId && docs.some((doc) => doc.id === activeId)) return;
    setActiveId(docs[0]?.id ?? "");
  }, [docs, activeId]);

  useEffect(() => {
    void invoke<AppInfo>("app_info")
      .then((info) => {
        setVersion(info.version);
        setClientBuild(info.client_build ?? "");
        setClientVersion(info.client_version ?? "");
      })
      .catch(() => {
        // Browser preview: keep the package version.
      });
  }, []);

  useEffect(() => {
    if (!toast) return;
    const timer = window.setTimeout(() => setToast(null), 4200);
    return () => window.clearTimeout(timer);
  }, [toast]);

  useEffect(() => {
    saveSettings(settings);
  }, [settings]);

  useEffect(() => {
    document.documentElement.dataset.accent = settings.accent;
  }, [settings.accent]);

  useEffect(() => {
    saveSession({ view, activeId });
  }, [view, activeId]);

  useEffect(() => {
    const timer = window.setTimeout(() => saveDocs(docs), 300);
    return () => window.clearTimeout(timer);
  }, [docs]);

  useEffect(() => {
    try {
      void getCurrentWindow()
        .setAlwaysOnTop(settings.topmost)
        .catch(() => {
          // Browser preview.
        });
    } catch {
      // Browser preview.
    }
  }, [settings.topmost]);

  useEffect(() => {
    void coreSetPipe(settings.pipeName).catch(() => {
      // Browser preview.
    });
  }, [settings.pipeName]);

  const showToast = useCallback((title: string, body: string) => {
    setToast({ id: Date.now(), title, body });
  }, []);

  const pushLog = useCallback((level: LogLevel, msg: string, run?: RunTag) => {
    setLogs((prev) => [...prev.slice(-(LOG_CAP - 1)), logLine(level, msg, undefined, run)]);
  }, []);

  // Requests whose lines already arrived as `output` events. Their result
  // still carries the full text for older cores, so skip it instead of
  // logging every line twice.
  const streamed = useRef<Set<number>>(new Set());
  const runNames = useRef<Map<number, string>>(new Map());

  useEffect(() => {
    let disposed = false;
    let unlisten: UnlistenFn | undefined;

    void (async () => {
      try {
        unlisten = await listenCore((event) => {
          if (event.type === "status") {
            setStatus(event.status);
            return;
          }

          if (event.type === "log") {
            pushLog(event.level, event.msg);
            return;
          }

          if (event.type === "toast") {
            setToast({ id: Date.now(), title: event.title, body: event.content });
            return;
          }

          if (event.type === "output") {
            streamed.current.add(event.id);
            const tag = { id: event.id, name: runNames.current.get(event.id) ?? "script" };
            pushLog(event.kind === "warn" ? "warn" : "info", event.text, tag);
            return;
          }

          const tag = { id: event.id, name: runNames.current.get(event.id) ?? "script" };
          if (event.error) {
            pushLog("error", event.error, tag);
          }
          if (!streamed.current.has(event.id)) {
            for (const line of event.output.split("\n")) {
              if (line) pushLog("info", line, tag);
            }
          }
          streamed.current.delete(event.id);
          if (event.ok && !event.error && !event.output) {
            pushLog("debug", `exec #${event.id} ok in ${event.ms} ms`, tag);
          }
        });

        if (disposed) {
          unlisten();
          unlisten = undefined;
          return;
        }

        const snapshot = await coreSnapshot();
        if (disposed) return;

        setStatus(snapshot.status);
        if (snapshot.logs.length > 0) {
          setLogs(snapshot.logs.map((entry) => logLine(entry.level, entry.msg, entry.ts)));
        }
      } catch {
        // Browser preview: there is no Tauri backend.
      }
    })();

    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [pushLog]);

  const updateSettings = useCallback((patch: Partial<Settings>) => {
    setSettings((prev) => ({ ...prev, ...patch }));
  }, []);

  const toggleFilter = useCallback((level: LogLevel) => {
    setSettings((prev) => ({
      ...prev,
      filters: { ...prev.filters, [level]: !prev.filters[level] },
    }));
  }, []);

  const updateSource = useCallback(
    (source: string) => {
      setDocs((prev) => prev.map((doc) => (doc.id === activeId ? { ...doc, source } : doc)));
    },
    [activeId],
  );

  const newScript = useCallback(() => {
    const doc = makeScript(docs.length + 1);
    setDocs((prev) => [...prev, doc]);
    setActiveId(doc.id);
    pushLog("info", `created ${doc.name}`);
  }, [docs.length, pushLog]);

  const closeDoc = useCallback(
    (id: string) => {
      const index = docs.findIndex((doc) => doc.id === id);
      const doc = docs[index];
      if (!doc) return;
      if (
        doc.source !== doc.saved &&
        !window.confirm(`Close ${doc.name}? Unsaved changes will be lost.`)
      ) {
        return;
      }

      const next = docs.filter((entry) => entry.id !== id);
      if (next.length === 0) {
        const fresh = makeScript(1);
        setDocs([fresh]);
        setActiveId(fresh.id);
        return;
      }

      setDocs(next);
      if (activeId === id) {
        setActiveId(next[Math.min(index, next.length - 1)].id);
      }
    },
    [docs, activeId],
  );

  const runSource = useCallback(
    async (name: string, source: string, opts?: { auto?: boolean }) => {
      if (!status.attached) {
        pushLog("warn", "no client session — start Roblox through the ZEND loader");
        if (!opts?.auto) {
          showToast(
            "No client attached",
            "Start Roblox with the ZEND loader, then run the script again.",
          );
        }
        return;
      }

      if (!opts?.auto && settings.clearOnRun) setLogs([]);

      try {
        const id = await coreExec(source, name);
        runNames.current.set(id, name);
        pushLog("debug", `exec #${id} · ${name}`, { id, name });
      } catch (err) {
        pushLog("error", errorText(err));
      }
    },
    [status.attached, settings.clearOnRun, pushLog, showToast],
  );

  const runActive = useCallback(() => {
    if (!activeDoc) return;
    void runSource(activeDoc.name, activeDoc.source);
  }, [activeDoc, runSource]);

  const runNamed = useCallback(
    (name: string, source: string) => runSource(name, source),
    [runSource],
  );

  const runAutoexec = useCallback(() => {
    if (!settings.autoexec.trim()) {
      pushLog("warn", "autoexec is empty");
      return;
    }
    setAutoexecAt(Date.now());
    void runSource("autoexec.lua", settings.autoexec);
  }, [settings.autoexec, runSource, pushLog]);

  const settingsRef = useRef(settings);
  settingsRef.current = settings;
  const runSourceRef = useRef(runSource);
  runSourceRef.current = runSource;
  const autoexecPid = useRef<number | null>(null);

  useEffect(() => {
    if (!status.attached || status.pid === null) {
      if (!status.attached) autoexecPid.current = null;
      return;
    }

    const current = settingsRef.current;
    const source = current.autoexec.trim();
    if (!current.autoexecOnAttach || source.length === 0) return;
    if (autoexecPid.current === status.pid) return;

    const pid = status.pid;
    const timer = window.setTimeout(() => {
      autoexecPid.current = pid;
      setAutoexecAt(Date.now());
      void runSourceRef.current("autoexec.lua", source, { auto: true });
    }, current.injectDelay);

    return () => window.clearTimeout(timer);
  }, [status.attached, status.pid]);

  const openScript = useCallback(
    (name: string, source: string) => {
      const safe =
        name
          .trim()
          .replace(/[\\/:*?"<>|]+/g, "-")
          .replace(/\s+/g, "-")
          .replace(/^-+|-+$/g, "") || "hub-script";
      const fileName = /\.(lua|luau)$/i.test(safe) ? safe : `${safe}.lua`;
      const doc = makeScript(docs.length + 1);
      const next = { ...doc, name: fileName, source, saved: "" };
      setDocs((prev) => [...prev, next]);
      setActiveId(next.id);
      setView("scripts");
      pushLog("info", `opened ${fileName} from hub`);
    },
    [docs.length, pushLog],
  );

  const saveActive = useCallback(() => {
    if (!activeDoc) return;

    setDocs((prev) =>
      prev.map((doc) => (doc.id === activeDoc.id ? { ...doc, saved: doc.source } : doc)),
    );
    pushLog("info", `saved ${activeDoc.name}`);
    showToast("Saved", `${activeDoc.name} saved locally.`);
  }, [activeDoc, pushLog, showToast]);

  const attach = useCallback(async () => {
    try {
      await coreConnect();
      pushLog("info", "attach requested");
    } catch (err) {
      const text = errorText(err);
      pushLog("warn", text);
      showToast("Attach failed", text);
    }
  }, [pushLog, showToast]);

  const eject = useCallback(async () => {
    try {
      await coreDetach();
      pushLog("info", "ejected from client");
      showToast("Ejected", "The pipe is closed. Relaunch the client to attach again.");
    } catch (err) {
      pushLog("error", errorText(err));
    }
  }, [pushLog, showToast]);

  const toggleTopmost = useCallback(() => {
    updateSettings({ topmost: !settings.topmost });
  }, [settings.topmost, updateSettings]);

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.defaultPrevented) return;

      const mod = event.ctrlKey || event.metaKey;
      if (event.key === "F5" || (mod && event.key === "Enter")) {
        event.preventDefault();
        void runActive();
        return;
      }
      if (mod && !event.shiftKey && event.key.toLowerCase() === "s") {
        event.preventDefault();
        saveActive();
        return;
      }
      if (mod && !event.shiftKey && event.key.toLowerCase() === "l") {
        event.preventDefault();
        setLogs([]);
        return;
      }
      if (mod && !event.shiftKey && event.key.toLowerCase() === "w") {
        event.preventDefault();
        if (activeDoc) closeDoc(activeDoc.id);
        return;
      }
      if (mod && event.key === "Tab") {
        event.preventDefault();
        if (docs.length < 2) return;
        const index = docs.findIndex((doc) => doc.id === activeId);
        if (index < 0) return;
        const next = (index + (event.shiftKey ? -1 : 1) + docs.length) % docs.length;
        setActiveId(docs[next].id);
      }
    };

    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [runActive, saveActive, closeDoc, activeDoc, docs, activeId]);

  const filters: LogFilters = settings.filters;
  const buildMismatch =
    clientBuild !== "" && status.build !== "" && status.build !== clientBuild;

  return (
    <div className="z-app">
      <TitleBar
        attached={status.attached}
        connecting={status.connecting}
        build={status.build}
        expectedBuild={clientBuild}
        pid={status.pid}
        topmost={settings.topmost}
        onAttach={() => void attach()}
        onEject={() => void eject()}
        onToggleTopmost={toggleTopmost}
      />

      <div className="z-main" data-view={view}>
        <Rail view={view} attached={status.attached} onChange={setView} />

        {view === "scripts" && (
          <>
            <ScriptList
              docs={docs}
              activeId={activeId}
              onSelect={setActiveId}
              onNew={newScript}
            />
            {activeDoc && (
              <Workbench
                docs={docs}
                activeDoc={activeDoc}
                onSelect={setActiveId}
                onNew={newScript}
                onClose={closeDoc}
                onChange={updateSource}
                onRun={runActive}
                logs={logs}
                onClear={() => setLogs([])}
                filters={filters}
                onToggleFilter={toggleFilter}
                wrap={settings.wrap}
              />
            )}
          </>
        )}

        {view === "hub" && (
          <HubPanel attached={status.attached} onRun={runNamed} onOpen={openScript} />
        )}
        {view === "autoexec" && (
          <AutoexecPanel
            settings={settings}
            onChange={updateSettings}
            status={status}
            lastRun={autoexecAt}
            onRun={runAutoexec}
          />
        )}
        {view === "console" && (
          <ConsolePanel
            logs={logs}
            onClear={() => setLogs([])}
            filters={filters}
            onToggleFilter={toggleFilter}
            full
          />
        )}
        {view === "settings" && (
          <SettingsPanel
            settings={settings}
            onChange={updateSettings}
            status={status}
            onOpenAutoexec={() => setView("autoexec")}
            onRunAutoexec={runAutoexec}
          />
        )}
      </div>

      <StatusBar
        build={status.build}
        expectedBuild={clientBuild}
        clientVersion={clientVersion}
        buildMismatch={buildMismatch}
        attached={status.attached}
        queue={status.queue}
        version={version}
        heat={status.heat}
        placeId={status.place_id}
        pingMs={status.ping_ms}
        pid={status.pid}
        channel={settings.channel}
        autoexecAt={autoexecAt}
      />

      {toast && (
        <div className="z-toast" role="status" key={toast.id}>
          <strong>{toast.title}</strong>
          <span>{toast.body}</span>
        </div>
      )}
    </div>
  );
}
