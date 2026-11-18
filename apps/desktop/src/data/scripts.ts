export interface ScriptDoc {
  id: string;
  name: string;
  source: string;
  saved: string;
}

const STORE_KEY = "zend.scripts.v1";

const WELCOME = `-- ZEND / welcome.lua
-- Ctrl+Enter runs the active tab.

local Players = game:GetService("Players")
local LocalPlayer = Players.LocalPlayer

print("hello from", LocalPlayer.Name)

task.spawn(function()
    for i = 1, 3 do
        print("tick", i)
        task.wait(0.25)
    end
end)
`;

function welcome(): ScriptDoc {
  return {
    id: "welcome",
    name: "welcome.lua",
    source: WELCOME,
    saved: WELCOME,
  };
}

let sequence = 0;

export function makeScript(index: number): ScriptDoc {
  sequence += 1;
  const source = "-- new script\n";
  return {
    id: `script-${Date.now()}-${sequence}`,
    name: `untitled-${index}.lua`,
    source,
    saved: "",
  };
}

function isDoc(value: unknown): value is ScriptDoc {
  if (!value || typeof value !== "object") return false;
  const doc = value as Record<string, unknown>;
  return (
    typeof doc.id === "string" &&
    typeof doc.name === "string" &&
    typeof doc.source === "string" &&
    typeof doc.saved === "string"
  );
}

export function loadDocs(): ScriptDoc[] {
  try {
    const raw = localStorage.getItem(STORE_KEY);
    if (raw) {
      const parsed: unknown = JSON.parse(raw);
      if (Array.isArray(parsed)) {
        const docs = parsed.filter(isDoc);
        if (docs.length > 0) return docs;
      }
    }
  } catch {
    // Corrupt local store: fall through to the welcome document.
  }
  return [welcome()];
}

export function saveDocs(docs: ScriptDoc[]): void {
  try {
    localStorage.setItem(STORE_KEY, JSON.stringify(docs));
  } catch {
    // Storage is best-effort; the editor still works in memory.
  }
}
