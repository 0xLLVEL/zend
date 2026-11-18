import { useState } from "react";
import { invoke, isTauri } from "@tauri-apps/api/core";
import type { CoreStatus } from "../lib/core";
import type { Accent, Channel, Settings } from "../data/settings";

interface SettingsPanelProps {
  settings: Settings;
  onChange: (patch: Partial<Settings>) => void;
  status: CoreStatus;
  appVersion: string;
  onToast: (title: string, body: string) => void;
  onOpenAutoexec: () => void;
  onRunAutoexec: () => void;
}

interface UpdateInfo {
  current: string;
  latest: string;
  available: boolean;
  url: string;
  notes: string;
  published: string;
  source: "remote" | "file";
}

type UpdateState =
  | { phase: "idle" }
  | { phase: "checking" }
  | { phase: "done"; info: UpdateInfo }
  | { phase: "error"; error: string };

interface ToggleProps {
  label: string;
  hint: string;
  checked: boolean;
  onChange: (value: boolean) => void;
}

export function Toggle({ label, hint, checked, onChange }: ToggleProps) {
  return (
    <div className="z-setting">
      <div className="z-setting__copy">
        <span className="z-setting__label">{label}</span>
        <span className="z-setting__hint">{hint}</span>
      </div>
      <button
        className={`z-switch ${checked ? "is-on" : ""}`}
        role="switch"
        aria-checked={checked}
        aria-label={label}
        onClick={() => onChange(!checked)}
      >
        <i />
      </button>
    </div>
  );
}

const THEMES: { id: Accent; label: string }[] = [
  { id: "ember", label: "Ember" },
  { id: "mint", label: "Mint" },
  { id: "azure", label: "Azure" },
];

function autoexecLines(source: string): number {
  const trimmed = source.trim();
  return trimmed.length === 0 ? 0 : trimmed.split(/\r?\n/).length;
}

export function SettingsPanel({
  settings,
  onChange,
  status,
  appVersion,
  onToast,
  onOpenAutoexec,
  onRunAutoexec,
}: SettingsPanelProps) {
  const [update, setUpdate] = useState<UpdateState>({ phase: "idle" });
  const lines = autoexecLines(settings.autoexec);
  const loaderValue = status.attached
    ? `Attached · pid ${status.pid ?? "?"}`
    : status.connecting
      ? "Attaching…"
      : "Waiting for a client";

  const checkForUpdates = async () => {
    if (!isTauri()) {
      setUpdate({ phase: "error", error: "run the desktop app to check for updates" });
      return;
    }
    setUpdate({ phase: "checking" });
    try {
      const info = await invoke<UpdateInfo>("update_check", {
        feed: settings.updateFeed.trim(),
      });
      setUpdate({ phase: "done", info });
      if (info.available) {
        onToast("Update available", `ZEND ${info.latest} is ready to download.`);
      }
    } catch (error) {
      setUpdate({ phase: "error", error: String(error) });
    }
  };

  const openDownload = async () => {
    if (update.phase !== "done" || !update.info.url) return;
    try {
      await invoke("update_open", { url: update.info.url });
    } catch (error) {
      onToast("Could not open download", String(error));
    }
  };

  const updateState = (() => {
    if (update.phase === "idle") return "Not checked";
    if (update.phase === "checking") return "Checking feed…";
    if (update.phase === "error") return `Check failed: ${update.error}`;
    const { info } = update;
    if (!info.available) return `Up to date (latest ${info.latest})`;
    if (!info.url) return `ZEND ${info.latest} is available, but the feed has no download URL`;
    return `ZEND ${info.latest} is available${info.published ? ` · ${info.published}` : ""}`;
  })();

  return (
    <div className="z-panel-scroll">
      <div className="z-panel">
        <header className="z-panel__head">
          <h2>Settings</h2>
          <p>Connection, execution and editor preferences. Everything is stored locally.</p>
        </header>

        <section className="z-section">
          <h3>Runtime</h3>
          <div className="z-row">
            <span className="z-row__label">Loader</span>
            <span className="z-row__value">{loaderValue}</span>
            <span className={`z-chip ${status.attached ? "" : "z-chip--muted"}`}>
              {status.attached ? "live" : "idle"}
            </span>
          </div>
          <div className="z-row">
            <span className="z-row__label">Autoexec</span>
            <span className="z-row__value">
              {lines === 0 ? "Not set" : `${lines} line${lines === 1 ? "" : "s"}`}
              {lines > 0 && (settings.autoexecOnAttach ? " · runs on attach" : " · manual only")}
            </span>
            <span className="z-row__actions">
              <button className="z-btn z-btn--small" onClick={onOpenAutoexec}>
                Open
              </button>
              <button
                className="z-btn z-btn--small"
                onClick={onRunAutoexec}
                disabled={lines === 0 || !status.attached}
              >
                Run
              </button>
            </span>
          </div>
          <div className="z-row">
            <span className="z-row__label">Protocol</span>
            <span className="z-row__value z-row__value--mono">v1 · named pipe</span>
            <span className="z-chip">ready</span>
          </div>
        </section>

        <section className="z-section">
          <h3>Connection</h3>
          <div className="z-field">
            <label className="z-field__copy" htmlFor="pipe-name">
              <span className="z-field__label">Pipe name override</span>
              <span className="z-field__hint">
                Blank follows the newest session file. A full{" "}
                <code>\\.\pipe\zend-…</code> path is accepted.
              </span>
            </label>
            <input
              id="pipe-name"
              className="z-input z-input--mono"
              value={settings.pipeName}
              spellCheck={false}
              placeholder="newest session"
              onChange={(event) => onChange({ pipeName: event.target.value })}
            />
          </div>
          <div className="z-field">
            <label className="z-field__copy" htmlFor="inject-delay">
              <span className="z-field__label">Inject delay</span>
              <span className="z-field__hint">
                Wait after attach before the autoexec runs. Applies to the next attach.
              </span>
            </label>
            <span className="z-input-wrap">
              <input
                id="inject-delay"
                className="z-input z-input--num"
                type="number"
                min={0}
                max={30000}
                step={100}
                value={settings.injectDelay}
                onChange={(event) =>
                  onChange({
                    injectDelay: Math.max(0, Math.min(30000, Number(event.target.value) || 0)),
                  })
                }
              />
              <span className="z-unit">ms</span>
            </span>
          </div>
          <div className="z-field">
            <label className="z-field__copy" htmlFor="channel">
              <span className="z-field__label">Loader channel</span>
              <span className="z-field__hint">A preference for the launch side; kept across updates.</span>
            </label>
            <select
              id="channel"
              className="z-select"
              value={settings.channel}
              onChange={(event) => onChange({ channel: event.target.value as Channel })}
            >
              <option value="stable">Stable</option>
              <option value="beta">Beta</option>
            </select>
          </div>
        </section>

        <section className="z-section">
          <h3>Editor</h3>
          <Toggle
            label="Wrap long lines"
            hint="Keep wide scripts inside the viewport."
            checked={settings.wrap}
            onChange={(value) => onChange({ wrap: value })}
          />
          <Toggle
            label="Clear output before each run"
            hint="Start every manual execution with a clean console."
            checked={settings.clearOnRun}
            onChange={(value) => onChange({ clearOnRun: value })}
          />
        </section>

        <section className="z-section">
          <h3>Appearance</h3>
          <div className="z-field">
            <span className="z-field__copy">
              <span className="z-field__label">Theme</span>
              <span className="z-field__hint">Accent used for selection, run state and the rail marker.</span>
            </span>
            <div className="z-themes" role="radiogroup" aria-label="Theme">
              {THEMES.map((theme) => (
                <button
                  key={theme.id}
                  className={`z-theme is-${theme.id} ${settings.accent === theme.id ? "is-active" : ""}`}
                  role="radio"
                  aria-checked={settings.accent === theme.id}
                  onClick={() => onChange({ accent: theme.id })}
                >
                  <i />
                  {theme.label}
                </button>
              ))}
            </div>
          </div>
          <Toggle
            label="Always on top"
            hint="Keep ZEND above the game window."
            checked={settings.topmost}
            onChange={(value) => onChange({ topmost: value })}
          />
        </section>

        <section className="z-section">
          <h3>Updates</h3>
          <div className="z-row">
            <span className="z-row__label">Version</span>
            <span className="z-row__value z-row__value--mono">{appVersion || "0.0.0"}</span>
            <span
              className={`z-chip ${
                update.phase === "done" && update.info.available ? "" : "z-chip--muted"
              }`}
            >
              {update.phase === "done" && update.info.available ? "available" : "current"}
            </span>
          </div>
          <div className="z-field">
            <label className="z-field__copy" htmlFor="update-feed">
              <span className="z-field__label">Update feed</span>
              <span className="z-field__hint">
                JSON document with the latest version, a download URL and release notes.
              </span>
            </label>
            <input
              id="update-feed"
              className="z-input z-input--mono"
              value={settings.updateFeed}
              spellCheck={false}
              placeholder="https://…/updates.json"
              onChange={(event) => onChange({ updateFeed: event.target.value })}
            />
          </div>
          <div className="z-field">
            <span className="z-field__copy">
              <span className="z-field__label">Status</span>
              <span className="z-field__hint">{updateState}</span>
            </span>
            <span className="z-row__actions">
              {update.phase === "done" && update.info.available && update.info.url ? (
                <button className="z-btn" onClick={openDownload}>
                  Download
                </button>
              ) : null}
              <button
                className="z-btn z-btn--small"
                onClick={checkForUpdates}
                disabled={update.phase === "checking"}
              >
                {update.phase === "checking" ? "Checking…" : "Check now"}
              </button>
            </span>
          </div>
          {update.phase === "done" && update.info.notes ? (
            <p className="z-panel__note z-update__notes">{update.info.notes}</p>
          ) : null}
        </section>

        <section className="z-section">
          <h3>Hotkeys</h3>
          <div className="z-keys">
            <span>Show / hide</span>
            <kbd>Ctrl Shift Z</kbd>
            <span>Run active script</span>
            <kbd>F5</kbd>
            <span>Save active script</span>
            <kbd>Ctrl S</kbd>
            <span>Close active tab</span>
            <kbd>Ctrl W</kbd>
            <span>Clear console</span>
            <kbd>Ctrl L</kbd>
          </div>
        </section>

        <p className="z-panel__note">
          Launch stays manual: start Roblox through the ZEND loader, then the shell attaches to its
          session. Eject closes the pipe from the UI side.
        </p>
      </div>
    </div>
  );
}
