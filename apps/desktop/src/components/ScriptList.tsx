import type { ScriptDoc } from "../data/scripts";

interface ScriptListProps {
  docs: ScriptDoc[];
  activeId: string;
  onSelect: (id: string) => void;
  onNew: () => void;
}

function FileGlyph() {
  return (
    <svg className="z-file" viewBox="0 0 16 16" aria-hidden="true">
      <path d="M4 1.75h5l3 3v9.5H4z" />
      <path d="M9 1.75v3h3" />
    </svg>
  );
}

export function ScriptList({ docs, activeId, onSelect, onNew }: ScriptListProps) {
  return (
    <aside className="z-list">
      <div className="z-list__head">
        <span>Scripts</span>
        <button className="z-list__new" onClick={onNew} title="New script" aria-label="New script">
          +
        </button>
      </div>

      <div className="z-list__group">
        <p className="z-list__label">Open</p>
        {docs.map((doc) => (
          <button
            key={doc.id}
            className={`z-list__item ${doc.id === activeId ? "is-active" : ""}`}
            onClick={() => onSelect(doc.id)}
            title={doc.name}
          >
            <FileGlyph />
            <span className="z-list__name">{doc.name}</span>
            {doc.source !== doc.saved && <i className="z-list__dirty" title="Unsaved changes" />}
          </button>
        ))}
      </div>

      <div className="z-list__foot">
        <span>{docs.length} open</span>
        <span>Luau</span>
      </div>
    </aside>
  );
}
