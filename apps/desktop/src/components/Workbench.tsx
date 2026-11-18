import type { ScriptDoc } from "../data/scripts";
import { EditorPane } from "./EditorPane";
import { ConsolePanel, type LogFilters, type LogLevel, type LogLine } from "./ConsolePanel";

interface WorkbenchProps {
  docs: ScriptDoc[];
  activeDoc: ScriptDoc;
  onSelect: (id: string) => void;
  onNew: () => void;
  onClose: (id: string) => void;
  onChange: (source: string) => void;
  onRun: () => void;
  logs: LogLine[];
  onClear: () => void;
  filters: LogFilters;
  onToggleFilter: (level: LogLevel) => void;
  wrap: boolean;
}

function FileGlyph() {
  return (
    <svg className="z-file" viewBox="0 0 16 16" aria-hidden="true">
      <path d="M4 1.75h5l3 3v9.5H4z" />
      <path d="M9 1.75v3h3" />
    </svg>
  );
}

function PlayGlyph() {
  return (
    <svg className="z-run__glyph" viewBox="0 0 10 10" aria-hidden="true">
      <path d="M2.5 1.5l6 3.5-6 3.5z" />
    </svg>
  );
}

export function Workbench({
  docs,
  activeDoc,
  onSelect,
  onNew,
  onClose,
  onChange,
  onRun,
  logs,
  onClear,
  filters,
  onToggleFilter,
  wrap,
}: WorkbenchProps) {
  return (
    <section className="z-workbench">
      <div className="z-tabs">
        <div className="z-tabs__scroll">
          {docs.map((doc) => (
            <div
              key={doc.id}
              className={`z-tab ${doc.id === activeDoc.id ? "is-active" : ""}`}
              onAuxClick={(event) => {
                if (event.button === 1) {
                  event.preventDefault();
                  onClose(doc.id);
                }
              }}
            >
              <button className="z-tab__main" onClick={() => onSelect(doc.id)} title={doc.name}>
                <FileGlyph />
                <span>{doc.name}</span>
                {doc.source !== doc.saved && <i className="z-tab__dirty" />}
              </button>
              <button
                className="z-tab__close"
                aria-label={`Close ${doc.name}`}
                title="Close tab"
                onClick={() => onClose(doc.id)}
              >
                ×
              </button>
            </div>
          ))}
          <button className="z-tab z-tab--new" onClick={onNew} title="New script" aria-label="New script">
            +
          </button>
        </div>

        <div className="z-tabs__actions">
          <span className="z-lang">Luau</span>
          <button className="z-run" onClick={onRun} title="Run (Ctrl+Enter)">
            <PlayGlyph />
            <span>Run</span>
            <kbd>Ctrl ↵</kbd>
          </button>
        </div>
      </div>

      <div className="z-editor-wrap">
        <EditorPane
          key={activeDoc.id}
          docId={activeDoc.id}
          value={activeDoc.source}
          wrap={wrap}
          onChange={onChange}
          onRun={onRun}
        />
      </div>

      <ConsolePanel
        logs={logs}
        onClear={onClear}
        filters={filters}
        onToggleFilter={onToggleFilter}
      />
    </section>
  );
}
