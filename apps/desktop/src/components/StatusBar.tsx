interface StatusBarProps {
  build: string;
  expectedBuild: string;
  clientVersion: string;
  buildMismatch: boolean;
  attached: boolean;
  queue: number;
  version: string;
  heat: number;
  placeId: number;
  pingMs: number | null;
  pid: number | null;
  channel: string;
  autoexecAt: number | null;
}

const TICKS = 12;

function clock(ms: number): string {
  return new Date(ms).toLocaleTimeString("en-GB", { hour12: false });
}

export function StatusBar({
  build,
  expectedBuild,
  clientVersion,
  buildMismatch,
  attached,
  queue,
  version,
  heat,
  placeId,
  pingMs,
  pid,
  channel,
  autoexecAt,
}: StatusBarProps) {
  const filled = Math.max(0, Math.min(TICKS, heat));

  return (
    <footer className="z-status">
      <span className="z-status__item">
        <i className={`z-status__dot ${attached ? "is-on" : ""}`} />
        {attached ? "core online" : "core offline"}
      </span>
      <span className="z-status__sep" />
      <span
        className={`z-status__item ${buildMismatch ? "is-warn" : ""}`}
        title={
          buildMismatch
            ? `client build ${build} does not match pinned ${expectedBuild}`
            : `client ${clientVersion || "unpinned"}`
        }
      >
        build <b>{build ? build.slice(0, 8) : "—"}</b>
        {buildMismatch && " !"}
      </span>
      {pid !== null && <span className="z-status__item">pid {pid}</span>}
      <span className="z-status__item">place {placeId > 0 ? placeId : "—"}</span>
      <span className={`z-status__item ${queue > 0 ? "is-busy" : ""}`}>queue {queue}</span>

      <span className="z-status__spacer" />

      <span className="z-status__item" title="Autoexec ran after the last attach">
        autoexec {autoexecAt === null ? "—" : clock(autoexecAt)}
      </span>
      <span className="z-status__item">{channel}</span>
      <span className="z-status__item">ping {pingMs === null ? "—" : `${pingMs} ms`}</span>
      <div className="z-heat" title="Hyperion detection heat from the loader's detlog ring">
        <span className="z-heat__label">heat</span>
        <span className="z-heat__ticks" aria-hidden="true">
          {Array.from({ length: TICKS }, (_, index) => (
            <i key={index} className={index < filled ? "is-on" : ""} />
          ))}
        </span>
        <span className="z-heat__value">{heat}</span>
      </div>
      <span className="z-status__version">v{version}</span>
    </footer>
  );
}
