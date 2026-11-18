import type { CoreStatus } from "../lib/core";
import type { Accent, Channel, Settings } from "../data/settings";

interface SettingsPanelProps {
  settings: Settings;
  onChange: (patch: Partial<Settings>) => void;
  status: CoreStatus;
  onOpenAutoexec: () => void;
  onRunAutoexec: () => void;
}

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
  onOpenAutoexec,
  onRunAutoexec,
}: SettingsPanelProps) {
  const lines = autoexecLines(settings.autoexec);
  const loaderValue = status.attached
    ? `Attached · pid ${status.pid ?? "?"}`
    : status.connecting
      ? "Attaching…"
      : "Waiting for a client";

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
