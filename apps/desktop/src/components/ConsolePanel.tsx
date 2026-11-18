import { Fragment, useEffect, useRef, useState } from "react";
import type { LogLevel } from "../lib/core";

export type { LogLevel };
export type LogFilters = Record<LogLevel, boolean>;

export interface RunTag {
  id: number;
  name: string;
}

export interface LogLine {
  id: number;
  ts: string;
  level: LogLevel;
  msg: string;
  run?: RunTag;
}

interface ConsolePanelProps {
  logs: LogLine[];
  onClear: () => void;
  filters: LogFilters;
  onToggleFilter: (level: LogLevel) => void;
  full?: boolean;
}

const LEVELS: LogLevel[] = ["info", "warn", "error", "debug"];

function TerminalGlyph() {
  return (
    <svg className="z-console__glyph" viewBox="0 0 16 16" aria-hidden="true">
      <rect x="1.5" y="2.5" width="13" height="11" rx="1" />
      <path d="M4 6l2 1.6L4 9.2M8.5 9.5h3" />
    </svg>
  );
}

export function ConsolePanel({ logs, onClear, filters, onToggleFilter, full = false }: ConsolePanelProps) {
  const body = useRef<HTMLDivElement>(null);
  const stick = useRef(true);
  const [copied, setCopied] = useState(false);

  const counts: Record<LogLevel, number> = { info: 0, warn: 0, error: 0, debug: 0 };
  for (const entry of logs) counts[entry.level] += 1;
  const visible = logs.filter((entry) => filters[entry.level]);

  useEffect(() => {
    const el = body.current;
    if (el && stick.current) el.scrollTop = el.scrollHeight;
  }, [logs, filters]);

  useEffect(() => {
    if (!copied) return;
    const timer = window.setTimeout(() => setCopied(false), 1400);
    return () => window.clearTimeout(timer);
  }, [copied]);

  async function copy() {
    const text = visible
      .map((entry) => `[${entry.ts}] ${entry.level.toUpperCase()} ${entry.msg}`)
      .join("\n");

    try {
      if (!navigator.clipboard?.writeText) throw new Error("clipboard unavailable");
      await navigator.clipboard.writeText(text);
    } catch {
      const area = document.createElement("textarea");
      area.value = text;
      area.style.position = "fixed";
      area.style.opacity = "0";
      document.body.appendChild(area);
      area.select();
      document.execCommand("copy");
      area.remove();
    }
    setCopied(true);
  }

  const seen = new Set<number>();

  return (
    <section className={`z-console ${full ? "z-console--full" : ""}`} aria-label="Output">
      <div className="z-console__head">
        <div className="z-console__title">
          <TerminalGlyph />
          <span>Output</span>
          <span className="z-console__count" title="Visible lines / total lines">
            {visible.length === logs.length ? logs.length : `${visible.length}/${logs.length}`}
          </span>
        </div>

        <div className="z-console__tools">
          <div className="z-levels" aria-label="Log level filters">
            {LEVELS.map((level) => (
              <button
                key={level}
                className={`z-level is-${level} ${filters[level] ? "is-on" : ""}`}
                aria-pressed={filters[level]}
                title={`${filters[level] ? "Hide" : "Show"} ${level} lines`}
                onClick={() => onToggleFilter(level)}
              >
                {level}
                <b>{counts[level]}</b>
              </button>
            ))}
          </div>
          <span className="z-console__sep" />
          <button
            className="z-console__action"
            onClick={() => void copy()}
            disabled={visible.length === 0}
          >
            {copied ? "Copied" : "Copy"}
          </button>
          <button
            className="z-console__action"
            onClick={onClear}
            disabled={logs.length === 0}
          >
            Clear
          </button>
        </div>
      </div>

      <div
        className="z-console__body"
        ref={body}
        onScroll={() => {
          const el = body.current;
          if (el) stick.current = el.scrollHeight - el.scrollTop - el.clientHeight < 28;
        }}
      >
        {logs.length === 0 ? (
          <p className="z-console__empty">No output yet. Run a script to see it here.</p>
        ) : visible.length === 0 ? (
          <p className="z-console__empty">All levels are hidden. Turn a filter back on.</p>
        ) : (
          visible.map((entry) => {
            const run = entry.run;
            const first = run !== undefined && !seen.has(run.id);
            if (run) seen.add(run.id);

            return (
              <Fragment key={entry.id}>
                {first && run && (
                  <div className="z-runhead">
                    <span className="z-runhead__id">#{run.id}</span>
                    <span className="z-runhead__name">{run.name}</span>
                    <span className="z-runhead__time">{entry.ts}</span>
                  </div>
                )}
                <div className={`z-line is-${entry.level}`}>
                  <span className="z-line__ts">{entry.ts}</span>
                  <span className="z-line__level">{entry.level}</span>
                  <span className="z-line__msg">{entry.msg}</span>
                </div>
              </Fragment>
            );
          })
        )}
      </div>
    </section>
  );
}
