export const PROTOCOL_VERSION = 1 as const;
export const MAX_FRAME_BYTES = 16 * 1024 * 1024;

export type Op =
  | "hello"
  | "exec"
  | "result"
  | "status"
  | "status.result"
  | "log"
  | "toast"
  | "ping"
  | "pong"
  | "detach";

export type LogLevel = "info" | "warn" | "error" | "debug";
export type ToastKind = "info" | "success" | "error";

export interface Envelope {
  v: typeof PROTOCOL_VERSION;
  id?: number;
  op: Op;
}

export interface Hello extends Envelope {
  op: "hello";
  token: string;
  pid: number;
}

export interface Exec extends Envelope {
  op: "exec";
  source: string;
  chunk: string;
}

export interface Result extends Envelope {
  op: "result";
  id: number;
  ok: boolean;
  output: string;
  error: string | null;
  ms: number;
}

export interface StatusRequest extends Envelope {
  op: "status";
  id: number;
}

export interface StatusResult extends Envelope {
  op: "status.result";
  id: number;
  build: string;
  attached: boolean;
  place_id: number;
  queue: number;
  heat: number;
}

export interface LogEvent extends Envelope {
  op: "log";
  level: LogLevel;
  msg: string;
  ts: number;
}

export interface ToastEvent extends Envelope {
  op: "toast";
  title: string;
  content: string;
  kind: ToastKind;
}

export interface Ping extends Envelope {
  op: "ping";
  id?: number;
}

export interface Pong extends Envelope {
  op: "pong";
  id?: number;
}

export interface Detach extends Envelope {
  op: "detach";
  id: number;
}

export type Request = Hello | Exec | StatusRequest | Ping | Detach;
export type Response = Result | StatusResult | Pong;
export type Event = LogEvent | ToastEvent;
export type Message = Request | Response | Event;

export interface SessionFile {
  v: typeof PROTOCOL_VERSION;
  pid: number;
  pipe: string;
  token: string;
  build: string;
  ts: number;
}
