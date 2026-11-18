export interface HubScript {
  id: string;
  name: string;
  description: string;
  tags: string[];
  source?: string;
  url?: string;
}

export interface HubManifest {
  name: string;
  version: string;
  scripts: HubScript[];
}

export interface HubState {
  manifest: HubManifest;
  origin: "builtin" | "custom";
  url: string;
  cache: Record<string, string>;
  updatedAt: number;
}

const HUB_KEY = "zend.hub.v1";

export const BUILTIN_MANIFEST: HubManifest = {
  name: "Zend Basics",
  version: "1",
  scripts: [
    {
      id: "session-info",
      name: "Session info",
      description: "Print the place, job, player count and executor identity for the current session.",
      tags: ["diagnostics"],
      source: `local Players = game:GetService("Players")
local lines = {
    "place    " .. tostring(game.PlaceId),
    "job      " .. tostring(game.JobId),
    "players  " .. tostring(#Players:GetPlayers()) .. "/" .. tostring(Players.MaxPlayers),
    "player   " .. tostring(Players.LocalPlayer and Players.LocalPlayer.Name or "none"),
    "identity " .. tostring(getidentity and getidentity() or "n/a"),
}
print(table.concat(lines, "\\n"))
`,
    },
    {
      id: "rejoin",
      name: "Rejoin server",
      description: "Teleport back into the current place and job id.",
      tags: ["server"],
      source: `local Players = game:GetService("Players")
local TeleportService = game:GetService("TeleportService")
local placeId, jobId = game.PlaceId, game.JobId
if placeId == 0 or jobId == "" then
    return print("rejoin: not connected to a live server")
end
print("rejoin:", placeId, jobId)
TeleportService:TeleportToPlaceInstance(placeId, jobId, Players.LocalPlayer)
`,
    },
    {
      id: "position",
      name: "Character position",
      description: "Print the HumanoidRootPart position as a Luau Vector3 constructor.",
      tags: ["character"],
      source: `local character = game:GetService("Players").LocalPlayer.Character
local root = character and character:FindFirstChild("HumanoidRootPart")
if not root then
    return print("position: character not spawned")
end
local p = root.Position
print(string.format("Vector3.new(%.2f, %.2f, %.2f)", p.X, p.Y, p.Z))
`,
    },
  ],
};

function builtinHub(): HubState {
  return {
    manifest: BUILTIN_MANIFEST,
    origin: "builtin",
    url: "",
    cache: {},
    updatedAt: 0,
  };
}

function stringList(value: unknown): string[] {
  return Array.isArray(value)
    ? value.filter((entry): entry is string => typeof entry === "string" && entry.trim().length > 0)
    : [];
}

export function parseManifest(value: unknown): HubManifest {
  if (!value || typeof value !== "object") {
    throw new Error("manifest must be a JSON object");
  }

  const raw = value as Record<string, unknown>;
  if (!Array.isArray(raw.scripts)) {
    throw new Error("manifest.scripts must be an array");
  }

  const scripts: HubScript[] = raw.scripts.map((entry, index) => {
    if (!entry || typeof entry !== "object") {
      throw new Error(`script ${index + 1} must be an object`);
    }

    const item = entry as Record<string, unknown>;
    const name = typeof item.name === "string" ? item.name.trim() : "";
    if (!name) throw new Error(`script ${index + 1} needs a name`);

    const source =
      typeof item.source === "string" && item.source.trim().length > 0 ? item.source : undefined;
    const url = typeof item.url === "string" && item.url.trim().length > 0 ? item.url.trim() : undefined;
    if (!source && !url) throw new Error(`script "${name}" needs source or url`);

    return {
      id:
        typeof item.id === "string" && item.id.trim().length > 0
          ? item.id.trim()
          : (url ?? `script-${index + 1}`),
      name,
      description: typeof item.description === "string" ? item.description : "",
      tags: stringList(item.tags),
      source,
      url,
    };
  });

  if (scripts.length === 0) throw new Error("manifest has no scripts");

  return {
    name: typeof raw.name === "string" && raw.name.trim() ? raw.name.trim() : "Untitled hub",
    version:
      typeof raw.version === "number"
        ? String(raw.version)
        : typeof raw.version === "string" && raw.version.trim()
          ? raw.version.trim()
          : "1",
    scripts,
  };
}

export function loadHub(): HubState {
  try {
    const raw = localStorage.getItem(HUB_KEY);
    if (raw) {
      const parsed = JSON.parse(raw) as Record<string, unknown>;
      const manifest = parseManifest(parsed.manifest);
      const cache: Record<string, string> = {};
      if (parsed.cache && typeof parsed.cache === "object") {
        for (const [key, value] of Object.entries(parsed.cache as Record<string, unknown>)) {
          if (typeof value === "string") cache[key] = value;
        }
      }
      return {
        manifest,
        origin: parsed.origin === "custom" ? "custom" : "builtin",
        url: typeof parsed.url === "string" ? parsed.url : "",
        cache,
        updatedAt: typeof parsed.updatedAt === "number" ? parsed.updatedAt : 0,
      };
    }
  } catch {
    // Corrupt cache: fall back to the built-in manifest.
  }
  return builtinHub();
}

export function saveHub(state: HubState): void {
  try {
    localStorage.setItem(HUB_KEY, JSON.stringify(state));
  } catch {
    // Best-effort.
  }
}

export function resetHub(cache: Record<string, string>): HubState {
  return { ...builtinHub(), cache };
}

export function cacheKey(script: HubScript): string {
  return script.url ?? script.id;
}

export function cachedSource(state: HubState, script: HubScript): string | undefined {
  if (script.source && script.source.trim().length > 0) return script.source;
  return state.cache[cacheKey(script)];
}

export async function fetchScript(script: HubScript): Promise<string> {
  if (script.source && script.source.trim().length > 0) return script.source;
  if (!script.url) throw new Error(`script "${script.name}" has no source or url`);

  const response = await fetch(script.url, { headers: { Accept: "text/plain" } });
  if (!response.ok) throw new Error(`fetch ${script.url}: HTTP ${response.status}`);
  return response.text();
}

export async function fetchManifest(url: string): Promise<HubManifest> {
  const response = await fetch(url, { headers: { Accept: "application/json" } });
  if (!response.ok) throw new Error(`fetch manifest: HTTP ${response.status}`);
  return parseManifest(await response.json());
}
