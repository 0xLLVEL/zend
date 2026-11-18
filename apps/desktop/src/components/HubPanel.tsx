import { useEffect, useState } from "react";
import {
  cachedSource,
  cacheKey,
  fetchManifest,
  fetchScript,
  loadHub,
  parseManifest,
  resetHub,
  saveHub,
  type HubScript,
  type HubState,
} from "../data/hub";

interface HubPanelProps {
  attached: boolean;
  onRun: (name: string, source: string) => void | Promise<void>;
  onOpen: (name: string, source: string) => void;
}

function SearchGlyph() {
  return (
    <svg className="z-search__glyph" viewBox="0 0 16 16" aria-hidden="true">
      <circle cx="7" cy="7" r="4.25" />
      <path d="M10.2 10.2L14 14" />
    </svg>
  );
}

function message(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

export function HubPanel({ attached, onRun, onOpen }: HubPanelProps) {
  const [hub, setHub] = useState<HubState>(loadHub);
  const [query, setQuery] = useState("");
  const [editorOpen, setEditorOpen] = useState(false);
  const [draft, setDraft] = useState("");
  const [urlDraft, setUrlDraft] = useState(hub.url);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);

  useEffect(() => {
    saveHub(hub);
  }, [hub]);

  const needle = query.trim().toLowerCase();
  const scripts = hub.manifest.scripts.filter((script) => {
    if (!needle) return true;
    return [script.name, script.description, ...script.tags].some((text) =>
      text.toLowerCase().includes(needle),
    );
  });

  function openEditor() {
    setDraft(JSON.stringify(hub.manifest, null, 2));
    setUrlDraft(hub.url);
    setError(null);
    setEditorOpen(true);
  }

  function applyDraft() {
    try {
      const manifest = parseManifest(JSON.parse(draft));
      setHub((prev) => ({
        manifest,
        origin: "custom",
        url: "",
        cache: prev.cache,
        updatedAt: Date.now(),
      }));
      setEditorOpen(false);
      setError(null);
    } catch (err) {
      setError(message(err));
    }
  }

  function useBuiltin() {
    setHub((prev) => resetHub(prev.cache));
    setEditorOpen(false);
    setError(null);
  }

  async function loadFromUrl() {
    const url = urlDraft.trim();
    if (!url) {
      setError("Enter a manifest URL first.");
      return;
    }

    setBusy("manifest");
    setError(null);
    try {
      const manifest = await fetchManifest(url);
      setHub((prev) => ({
        manifest,
        origin: "custom",
        url,
        cache: prev.cache,
        updatedAt: Date.now(),
      }));
      setEditorOpen(false);
    } catch (err) {
      setError(message(err));
    } finally {
      setBusy(null);
    }
  }

  async function reload() {
    if (!hub.url) {
      setError("This manifest is local. Edit it or fetch one by URL.");
      return;
    }

    setBusy("manifest");
    setError(null);
    try {
      const manifest = await fetchManifest(hub.url);
      setHub((prev) => ({ ...prev, manifest, updatedAt: Date.now() }));
    } catch (err) {
      setError(message(err));
    } finally {
      setBusy(null);
    }
  }

  async function resolve(script: HubScript): Promise<string> {
    const cached = cachedSource(hub, script);
    if (cached !== undefined) return cached;

    const source = await fetchScript(script);
    setHub((prev) => ({
      ...prev,
      cache: { ...prev.cache, [cacheKey(script)]: source },
    }));
    return source;
  }

  async function run(script: HubScript) {
    setBusy(`run:${script.id}`);
    setError(null);
    try {
      const source = await resolve(script);
      await onRun(script.name, source);
    } catch (err) {
      setError(message(err));
    } finally {
      setBusy(null);
    }
  }

  async function open(script: HubScript) {
    setBusy(`open:${script.id}`);
    setError(null);
    try {
      const source = await resolve(script);
      onOpen(script.name, source);
    } catch (err) {
      setError(message(err));
    } finally {
      setBusy(null);
    }
  }

  const origin = hub.origin === "builtin" ? "Built-in manifest" : hub.url || "Custom JSON";

  return (
    <div className="z-panel-scroll">
      <div className="z-panel z-panel--wide">
        <header className="z-panel__head z-panel__head--row">
          <div>
            <h2>{hub.manifest.name}</h2>
            <p>
              {hub.manifest.scripts.length} script
              {hub.manifest.scripts.length === 1 ? "" : "s"} · {origin}
            </p>
          </div>
          <div className="z-head__actions">
            <span className={`z-chip ${attached ? "" : "z-chip--muted"}`}>
              {attached ? "client attached" : "client offline"}
            </span>
            <button className="z-btn" onClick={openEditor}>
              Manifest
            </button>
            <button className="z-btn" onClick={() => void reload()} disabled={!hub.url || busy !== null}>
              Reload
            </button>
          </div>
        </header>

        <div className="z-hub__bar">
          <label className="z-search">
            <SearchGlyph />
            <input
              value={query}
              placeholder="Search scripts, tags…"
              spellCheck={false}
              onChange={(event) => setQuery(event.target.value)}
            />
            {query && (
              <button className="z-search__clear" aria-label="Clear search" onClick={() => setQuery("")}>
                ×
              </button>
            )}
          </label>
          <span className="z-hub__hint">
            {hub.origin === "custom" && hub.updatedAt > 0
              ? `updated ${new Date(hub.updatedAt).toLocaleTimeString("en-GB", { hour12: false })}`
              : "inline sources, no network needed"}
          </span>
        </div>

        {editorOpen && (
          <section className="z-hub__editor">
            <div className="z-hub__editor-head">
              <h3>Manifest JSON</h3>
              <span>scripts need a name and either source or url</span>
            </div>
            <textarea
              className="z-textarea"
              value={draft}
              spellCheck={false}
              onChange={(event) => setDraft(event.target.value)}
            />
            <div className="z-hub__editor-foot">
              <div className="z-urlrow">
                <input
                  className="z-input"
                  value={urlDraft}
                  spellCheck={false}
                  placeholder="https://example.com/hub.json"
                  onChange={(event) => setUrlDraft(event.target.value)}
                />
                <button
                  className="z-btn"
                  onClick={() => void loadFromUrl()}
                  disabled={busy !== null}
                >
                  {busy === "manifest" ? "Fetching" : "Fetch URL"}
                </button>
              </div>
              <div className="z-head__actions">
                <button className="z-btn" onClick={() => setEditorOpen(false)}>
                  Cancel
                </button>
                <button className="z-btn" onClick={useBuiltin}>
                  Built-in
                </button>
                <button className="z-btn z-btn--primary" onClick={applyDraft}>
                  Apply JSON
                </button>
              </div>
            </div>
          </section>
        )}

        {error && (
          <p className="z-hub__error" role="alert">
            {error}
          </p>
        )}

        <div className="z-hub__list">
          {scripts.length === 0 ? (
            <p className="z-hub__empty">No scripts match “{query}”.</p>
          ) : (
            scripts.map((script) => {
              const cached =
                script.source === undefined && cachedSource(hub, script) !== undefined;
              const running = busy === `run:${script.id}`;
              const opening = busy === `open:${script.id}`;

              return (
                <article className="z-hub__item" key={script.id}>
                  <div className="z-hub__copy">
                    <div className="z-hub__name">
                      {script.name}
                      {cached && <span className="z-hub__cache">cached</span>}
                    </div>
                    <p>{script.description || "No description."}</p>
                    {script.tags.length > 0 && (
                      <div className="z-hub__tags">
                        {script.tags.map((tag) => (
                          <span key={tag}>{tag}</span>
                        ))}
                      </div>
                    )}
                  </div>
                  <div className="z-hub__actions">
                    <button
                      className="z-btn z-btn--small"
                      onClick={() => void open(script)}
                      disabled={busy !== null}
                    >
                      {opening ? "Opening" : "Open"}
                    </button>
                    <button
                      className="z-btn z-btn--small z-btn--primary"
                      onClick={() => void run(script)}
                      disabled={busy !== null || !attached}
                      title={!attached ? "Attach a client first" : undefined}
                    >
                      {running ? "Running" : "Run"}
                    </button>
                  </div>
                </article>
              );
            })
          )}
        </div>

        <p className="z-panel__note">
          Remote scripts are fetched once and cached in local storage. Open copies a script into the
          editor without running it.
        </p>
      </div>
    </div>
  );
}
