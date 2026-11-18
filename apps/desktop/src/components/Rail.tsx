import type { ReactNode } from "react";

export type View = "scripts" | "hub" | "autoexec" | "console" | "settings";

interface RailProps {
  view: View;
  attached: boolean;
  onChange: (view: View) => void;
}

const stroke = {
  fill: "none",
  stroke: "currentColor",
  strokeWidth: 1.4,
  strokeLinecap: "round" as const,
  strokeLinejoin: "round" as const,
};

const ITEMS: { id: View; label: string; icon: ReactNode }[] = [
  {
    id: "scripts",
    label: "Scripts",
    icon: (
      <svg viewBox="0 0 20 20" aria-hidden="true" {...stroke}>
        <path d="M5 2.5h6l4 4v11H5z" />
        <path d="M11 2.5v4h4M7.5 10h5M7.5 13h3.5" />
      </svg>
    ),
  },
  {
    id: "hub",
    label: "Hub",
    icon: (
      <svg viewBox="0 0 20 20" aria-hidden="true" {...stroke}>
        <rect x="3" y="3" width="5.5" height="5.5" rx="0.8" />
        <rect x="11.5" y="3" width="5.5" height="5.5" rx="0.8" />
        <rect x="3" y="11.5" width="5.5" height="5.5" rx="0.8" />
        <rect x="11.5" y="11.5" width="5.5" height="5.5" rx="0.8" />
      </svg>
    ),
  },
  {
    id: "autoexec",
    label: "Autoexec",
    icon: (
      <svg viewBox="0 0 20 20" aria-hidden="true" {...stroke}>
        <path d="M11 2.5L4.5 11H9l-1 6.5L15.5 9H11z" />
      </svg>
    ),
  },
  {
    id: "console",
    label: "Console",
    icon: (
      <svg viewBox="0 0 20 20" aria-hidden="true" {...stroke}>
        <rect x="2.5" y="4" width="15" height="12" rx="1.2" />
        <path d="M5.5 8l2.5 2-2.5 2M10.5 12h4" />
      </svg>
    ),
  },
  {
    id: "settings",
    label: "Settings",
    icon: (
      <svg viewBox="0 0 20 20" aria-hidden="true" {...stroke}>
        <path d="M3.5 6.5h13M3.5 13.5h13" />
        <circle cx="8" cy="6.5" r="1.8" />
        <circle cx="12.5" cy="13.5" r="1.8" />
      </svg>
    ),
  },
];

export function Rail({ view, attached, onChange }: RailProps) {
  return (
    <nav className="z-rail" aria-label="Workspace">
      {ITEMS.map((item) => (
        <button
          key={item.id}
          className={`z-rail__btn ${view === item.id ? "is-active" : ""}`}
          aria-label={item.label}
          aria-pressed={view === item.id}
          title={item.label}
          onClick={() => onChange(item.id)}
        >
          {item.icon}
        </button>
      ))}

      <div className="z-rail__foot" title={attached ? "Core attached" : "Core offline"}>
        <i className={`z-rail__dot ${attached ? "is-on" : ""}`} />
        <span>v1</span>
      </div>
    </nav>
  );
}
