/** Vibe Bridge plugin API v1. Local plugins execute in the desktop process. */
/** Pixel image supplied by the bridge, not a firmware animal-template name. */
export interface AgentIcon {
  format: 'indexed4';
  width: number; // integer 1..48
  height: number; // integer 1..48
  palette: string[]; // 1..16 colors, RRGGBBAA without #
  data: string; // canonical base64: 4-bit linear indices, high nibble first
  background: string; // #RRGGBB
  accent: string; // #RRGGBB
}
/** Legacy preset strings are expanded on the bridge before transmission. */
export type Icon = AgentIcon | 'fox' | 'rabbit' | 'owl' | 'panda';
export type Settings = Record<string, string>;
export interface Capabilities {
  session: 'native' | 'external_unscoped';
  model: boolean;
  cancel: boolean;
  progress: boolean;
}
export interface Job {
  id: string;
  provider: string;
  instruction: string;
  vibeSessionId: string;
  sessionId?: string;
  [key: string]: unknown;
}
export interface Project { id: string; label: string; path: string }
export interface PluginConfig {
  locale?: 'zh-CN' | 'en';
  stateDir: string;
  models?: Record<string, string>;
  pluginSettings?: Record<string, Record<string, unknown>>;
  [key: string]: unknown;
}
export interface RunContext {
  job: Job;
  project: Project;
  signal: AbortSignal;
  config: PluginConfig;
  model?: string;
  onProgress: (text: string) => void;
  onSession: (sessionId: string) => void;
}
export interface RunResult {
  status: 'completed' | 'handed_off';
  result: string;
  gitSnapshot?: string;
  handoff?: { cursor: string; readState: string; replyCount?: number; [key: string]: unknown };
}
export interface CodingAgentPlugin {
  apiVersion: 1;
  kind: 'coding-agent';
  id: string;
  label: string;
  icon?: Icon;
  capabilities: Capabilities;
  model?: { default: string; required: boolean };
  sessionIdPattern?: RegExp;
  probe: (config: PluginConfig) => { available: boolean; reason?: string; mode?: string };
  run: (context: RunContext) => Promise<RunResult>;
  readUpdates?: (context: { job: Job; config: PluginConfig }) => Promise<{
    handoff: { cursor: string; readState: string; replyCount?: number };
    appendResult: string;
  }>;
}
export interface BoardEvent {
  name: string;
  fields: Record<string, string>;
}
export interface BoardCommand {
  name: string;
  fields?: Record<string, string>;
}
export interface QueuedBoardCommand extends BoardCommand {
  id: string;
  fields: Record<string, string>;
}
/**
 * Bridge plugin: subscribes to board events. Event delivery and command
 * delivery are separate. Return commands or call send(); both enqueue for a
 * later pull. send() stays valid after onEvent returns. Pass a PCM WAV
 * buffer as the second argument only for audio.play; the board fetches those
 * bytes with the command.
 */
export interface BoardPlugin {
  apiVersion: 1;
  kind: 'board-plugin';
  id: string;
  label: string;
  events: string[];
  commands: string[];
  probe: (config: PluginConfig) => { available: boolean; reason?: string };
  onConnect?: (context: {
    deviceId: string;
    config: PluginConfig;
    send: (command: BoardCommand, audio?: Uint8Array) => QueuedBoardCommand;
  }) => Promise<void> | void;
  onEvent: (context: {
    event: BoardEvent;
    config: PluginConfig;
    deviceId: string;
    send: (command: BoardCommand, audio?: Uint8Array) => QueuedBoardCommand;
  }) => Promise<{ commands?: BoardCommand[] } | void> | { commands?: BoardCommand[] } | void;
}
export type VibePlugin = CodingAgentPlugin | BoardPlugin;
