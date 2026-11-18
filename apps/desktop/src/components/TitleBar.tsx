import { getCurrentWindow } from "@tauri-apps/api/window";

interface TitleBarProps {
  attached: boolean;
  connecting: boolean;
  build: string;
  expectedBuild: string;
  pid: number | null;
  topmost: boolean;
  onAttach: () => void;
  onEject: () => void;
  onToggleTopmost: () => void;
}

function BrandMark() {
  return (
    <svg className="z-brand__mark" viewBox="0 0 16 16" aria-hidden="true">
      <rect x="1" y="6" width="3" height="9" rx="0.5" fill="currentColor" opacity="0.45" />
      <rect x="6.5" y="1" width="3" height="14" rx="0.5" fill="currentColor" />
      <rect x="12" y="4" width="3" height="11" rx="0.5" fill="currentColor" opacity="0.7" />
    </svg>
  );
}

function PinGlyph() {
  return (
    <svg viewBox="0 0 16 16" aria-hidden="true">
      <path d="M6 1.75h4M6.75 1.75v3.4L5 7.6v1h6v-1L9.25 5.15v-3.4M8 8.6v5.65" />
    </svg>
  );
}

function windowAction(action: "minimize" | "maximize" | "close") {
  try {
    const win = getCurrentWindow();
    if (action === "minimize") void win.minimize();
    if (action === "maximize") void win.toggleMaximize();
    if (action === "close") void win.close();
  } catch {
    // Browser preview has no Tauri window.
  }
}

export function TitleBar({
  attached,
  connecting,
  build,
  expectedBuild,
  pid,
  topmost,
  onAttach,
  onEject,
  onToggleTopmost,
}: TitleBarProps) {
  const short = build ? `${build.slice(0, 8)}…` : "no build";
  const mismatch = expectedBuild !== "" && build !== "" && build !== expectedBuild;

  return (
    <header
      className="z-titlebar"
      data-tauri-drag-region
      onDoubleClick={(event) => {
        if (event.target === event.currentTarget) windowAction("maximize");
      }}
    >
      <div className="z-brand" data-tauri-drag-region>
        <BrandMark />
        <span className="z-brand__word">ZEND</span>
      </div>

      <div className="z-titlebar__meta" data-tauri-drag-region>
        <span
          className={`z-build ${mismatch ? "is-warn" : ""}`}
          title={
            mismatch
              ? `client build ${build} does not match pinned ${expectedBuild}`
              : `client build ${build || "none"} · pinned ${expectedBuild || "none"}`
          }
        >
          {short}
        </span>
        <span className={`z-attach ${attached ? "is-on" : ""}`} title={pid ? `pid ${pid}` : undefined}>
          <i className="z-attach__dot" />
          {attached ? "attached" : connecting ? "attaching" : "no client"}
        </span>
      </div>

      <div className="z-titlebar__spacer" data-tauri-drag-region />

      <div className="z-titlebar__actions">
        {attached ? (
          <button className="z-titlebtn" onClick={onEject} title="Detach the UI from the client core">
            Eject
          </button>
        ) : (
          <button
            className="z-titlebtn"
            onClick={onAttach}
            disabled={connecting}
            title="Attach to the newest core session"
          >
            {connecting ? "Attaching" : "Attach"}
          </button>
        )}
        <button
          className={`z-pin ${topmost ? "is-on" : ""}`}
          aria-label="Always on top"
          aria-pressed={topmost}
          title="Always on top"
          onClick={onToggleTopmost}
        >
          <PinGlyph />
        </button>
      </div>

      <div className="z-win">
        <button
          className="z-win__btn"
          aria-label="Minimize"
          title="Minimize"
          onClick={() => windowAction("minimize")}
        >
          <svg viewBox="0 0 10 10" aria-hidden="true">
            <path d="M2 5h6" />
          </svg>
        </button>
        <button
          className="z-win__btn"
          aria-label="Maximize"
          title="Maximize"
          onClick={() => windowAction("maximize")}
        >
          <svg viewBox="0 0 10 10" aria-hidden="true">
            <rect x="2.4" y="2.4" width="5.2" height="5.2" />
          </svg>
        </button>
        <button
          className="z-win__btn z-win__btn--close"
          aria-label="Close"
          title="Close"
          onClick={() => windowAction("close")}
        >
          <svg viewBox="0 0 10 10" aria-hidden="true">
            <path d="M2.5 2.5l5 5M7.5 2.5l-5 5" />
          </svg>
        </button>
      </div>
    </header>
  );
}
