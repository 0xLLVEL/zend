import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";

export type LogLevel = "info" | "warn" | "error" | "debug";

export interface CoreStatus {
  attached: boolean;
  connecting: boolean;
  build: string;
  place_id: number;
  queue: number;
  heat: number;
  ping_ms: number | null;
  pid: number | null;
  error: string | null;
}

export interface CoreLog {
  level: LogLevel;
  msg: string;
  ts: number;
}

export interface Snapshot {
  status: CoreStatus;
  logs: CoreLog[];
}

export type CoreEvent =
  | { type: "status"; status: CoreStatus }
  | { type: "log"; level: LogLevel; msg: string }
  | { type: "result"; id: number; ok: boolean; output: string; error: string | null; ms: number }
  | { type: "output"; id: number; kind: "print" | "warn"; text: string }
  | { type: "toast"; title: string; content: string; kind: string };

export const OFFLINE: CoreStatus = {
  attached: false,
  connecting: false,
  build: "",
  place_id: 0,
  queue: 0,
  heat: 0,
  ping_ms: null,
  pid: null,
  error: null,
};

export function coreSnapshot(): Promise<Snapshot> {
  return invoke<Snapshot>("core_snapshot");
}

export function coreExec(source: string, chunk: string): Promise<number> {
  return invoke<number>("core_exec", { source, chunk });
}

export function coreConnect(): Promise<void> {
  return invoke<void>("core_connect");
}

export function coreSetPipe(name: string): Promise<void> {
  return invoke<void>("core_set_pipe", { name });
}

export function coreDetach(): Promise<void> {
  return invoke<void>("core_detach");
}

export function listenCore(handler: (event: CoreEvent) => void): Promise<UnlistenFn> {
  return listen<CoreEvent>("core-event", (event) => handler(event.payload));
}
