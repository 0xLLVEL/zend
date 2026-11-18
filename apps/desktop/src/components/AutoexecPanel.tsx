import { useState } from "react";
import type { CoreStatus } from "../lib/core";
import type { Settings } from "../data/settings";
import { EditorPane } from "./EditorPane";
import { Toggle } from "./SettingsPanel";

interface AutoexecPanelProps {
  settings: Settings;
  onChange: (patch: Partial<Settings>) => void;
  status: CoreStatus;
  lastRun: number | null;
  onRun: () => void;
}

function autoexecLines(source: string): number {
  const trimmed = source.trim();
  return trimmed.length === 0 ? 0 : trimmed.split(/\r?\n/).length;
}

function clock(ms: number): string {
  return new Date(ms).toLocaleTimeString("en-GB", { hour12: false });
}

export function AutoexecPanel({
  settings,
  onChange,
  status,
  lastRun,
  onRun,
}: AutoexecPanelProps) {
  const [revision, setRevision] = useState(0);
  const lines = autoexecLines(settings.autoexec);

  function clear() {
    if (settings.autoexec.trim() && !window.confirm("Clear the autoexec script?")) return;
    onChange({ autoexec: "" });
    setRevision((value) => value + 1);
  }

  return (
    <div className="z-panel-scroll">
      <div className="z-panel z-panel--wide">
        <header className="z-panel__head z-panel__head--row">
          <div>
            <h2>Autoexec</h2>
            <p>Runs once for each new client session. Output lands in the console tagged autoexec.lua.</p>
          </div>
          <div className="z-head__actions">
            <span className={`z-chip ${status.attached ? "" : "z-chip--muted"}`}>
              {status.attached ? "client attached" : "client offline"}
            </span>
            <button className="z-btn" onClick={onRun} disabled={lines === 0 || !status.attached}>
              Run now
            </button>
            <button className="z-btn" onClick={clear} disabled={lines === 0}>
              Clear
            </button>
          </div>
        </header>

        <section className="z-section z-autoexec">
          <div className="z-autoexec__top">
            <Toggle
              label="Run on attach"
              hint="Fire once after the inject delay for every new session."
              checked={settings.autoexecOnAttach}
              onChange={(value) => onChange({ autoexecOnAttach: value })}
            />
            <label className="z-autoexec__delay" htmlFor="autoexec-delay">
              <span>Delay</span>
              <input
                id="autoexec-delay"
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
            </label>
          </div>

          <div className="z-autoexec__editor">
            <EditorPane
              key={`autoexec-${revision}`}
              docId="autoexec"
              value={settings.autoexec}
              wrap={settings.wrap}
              onChange={(source) => onChange({ autoexec: source })}
              onRun={onRun}
            />
          </div>

          <footer className="z-autoexec__foot">
            <span>
              {lines === 0
                ? "Empty. Add initialization code above."
                : `${lines} line${lines === 1 ? "" : "s"}`}
            </span>
            <span>
              {settings.autoexecOnAttach ? `delay ${settings.injectDelay} ms` : "manual only"}
            </span>
            <span>last run {lastRun === null ? "—" : clock(lastRun)}</span>
          </footer>
        </section>
      </div>
    </div>
  );
}
