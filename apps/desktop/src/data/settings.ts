export type Accent = "ember" | "mint" | "azure";
export type Channel = "stable" | "beta";

export type LogFilters = Record<"info" | "warn" | "error" | "debug", boolean>;

export interface Settings {
  wrap: boolean;
  clearOnRun: boolean;
  autoexec: string;
  autoexecOnAttach: boolean;
  injectDelay: number;
  pipeName: string;
  updateFeed: string;
  accent: Accent;
  topmost: boolean;
  channel: Channel;
  filters: LogFilters;
}

export interface SessionState {
  view: string;
  activeId: string;
}

const SETTINGS_KEY = "zend.settings.v1";
const SESSION_KEY = "zend.session.v1";

export const DEFAULT_SETTINGS: Settings = {
  wrap: true,
  clearOnRun: false,
  autoexec: "",
  autoexecOnAttach: true,
  injectDelay: 1000,
  pipeName: "",
  updateFeed: "https://raw.githubusercontent.com/0xLLVEL/zend/main/updates.json",
  accent: "ember",
  topmost: false,
  channel: "stable",
  filters: { info: true, warn: true, error: true, debug: true },
};

const ACCENTS: Accent[] = ["ember", "mint", "azure"];
const CHANNELS: Channel[] = ["stable", "beta"];

function clampDelay(value: number): number {
  if (!Number.isFinite(value)) return DEFAULT_SETTINGS.injectDelay;
  return Math.max(0, Math.min(30000, Math.round(value)));
}

function readFilters(value: unknown): LogFilters {
  if (!value || typeof value !== "object") return { ...DEFAULT_SETTINGS.filters };
  const raw = value as Record<string, unknown>;
  return {
    info: typeof raw.info === "boolean" ? raw.info : true,
    warn: typeof raw.warn === "boolean" ? raw.warn : true,
    error: typeof raw.error === "boolean" ? raw.error : true,
    debug: typeof raw.debug === "boolean" ? raw.debug : true,
  };
}

export function loadSettings(): Settings {
  try {
    const raw = localStorage.getItem(SETTINGS_KEY);
    if (!raw) return { ...DEFAULT_SETTINGS, filters: { ...DEFAULT_SETTINGS.filters } };

    const parsed = JSON.parse(raw) as Record<string, unknown>;
    return {
      wrap: typeof parsed.wrap === "boolean" ? parsed.wrap : DEFAULT_SETTINGS.wrap,
      clearOnRun:
        typeof parsed.clearOnRun === "boolean"
          ? parsed.clearOnRun
          : DEFAULT_SETTINGS.clearOnRun,
      autoexec: typeof parsed.autoexec === "string" ? parsed.autoexec : "",
      autoexecOnAttach:
        typeof parsed.autoexecOnAttach === "boolean"
          ? parsed.autoexecOnAttach
          : DEFAULT_SETTINGS.autoexecOnAttach,
      injectDelay:
        typeof parsed.injectDelay === "number"
          ? clampDelay(parsed.injectDelay)
          : DEFAULT_SETTINGS.injectDelay,
      pipeName: typeof parsed.pipeName === "string" ? parsed.pipeName : "",
      updateFeed:
        typeof parsed.updateFeed === "string"
          ? parsed.updateFeed
          : DEFAULT_SETTINGS.updateFeed,
      accent: ACCENTS.includes(parsed.accent as Accent)
        ? (parsed.accent as Accent)
        : DEFAULT_SETTINGS.accent,
      topmost: typeof parsed.topmost === "boolean" ? parsed.topmost : false,
      channel: CHANNELS.includes(parsed.channel as Channel)
        ? (parsed.channel as Channel)
        : DEFAULT_SETTINGS.channel,
      filters: readFilters(parsed.filters),
    };
  } catch {
    return { ...DEFAULT_SETTINGS, filters: { ...DEFAULT_SETTINGS.filters } };
  }
}

export function saveSettings(settings: Settings): void {
  try {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
  } catch {
    // Storage is best-effort; the session keeps working in memory.
  }
}

export function loadSession(): SessionState {
  try {
    const raw = localStorage.getItem(SESSION_KEY);
    if (raw) {
      const parsed = JSON.parse(raw) as Record<string, unknown>;
      return {
        view: typeof parsed.view === "string" ? parsed.view : "scripts",
        activeId: typeof parsed.activeId === "string" ? parsed.activeId : "",
      };
    }
  } catch {
    // Corrupt session: start on the default view.
  }
  return { view: "scripts", activeId: "" };
}

export function saveSession(session: SessionState): void {
  try {
    localStorage.setItem(SESSION_KEY, JSON.stringify(session));
  } catch {
    // Best-effort.
  }
}
