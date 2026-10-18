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
/** Streaming JSONL event parser shared by CLI coding agents. */
export interface CliEventStream {
  write(chunk: string): void;
  finish(): { result: string; hasAssistantText: boolean; error: string; rawTail: string };
}
/** Per-CLI adapter handed to runCli: command line plus event interpretation. */
export interface CliAdapter {
  label: string;
  createEvents: (onProgress?: (text: string) => void, onSession?: (sessionId: string) => void) => CliEventStream;
  command: (options: { cwd: string; instruction: string; config: PluginConfig; model?: string; sessionId?: string }) => {
    command: string; args: string[]; stdin?: string;
  };
  classifyExit?: (context: {
    job: Job; project: Project; stderr: string;
    output: { result: string; hasAssistantText: boolean; error: string; rawTail: string };
    startingGitSnapshot: string;
  }) => Error | void;
}
/**
 * Cross-platform helpers the bridge hands to every plugin. Use these instead of
 * hand-rolling PATH lookups or spawning npm: on Windows npm is npm.cmd, which
 * Node 20+ refuses to spawn without a shell, and PATH lookups must walk PATHEXT.
 * Also includes the CLI toolbox (probeCli/runCli/event parsing) used by CLI
 * coding-agent plugins; guardrails and the stuck watchdog live in runCli.
 */
export interface PluginRuntime {
  /** process.platform of the bridge, e.g. 'darwin' | 'linux' | 'win32'. */
  platform: string;
  isWindows: boolean;
  /** Absolute path of a command on PATH, or '' when it is not installed. */
  findCommand: (command: string, options?: { env?: NodeJS.ProcessEnv }) => string;
  /** Whether a command is installed. */
  hasCommand: (command: string, options?: { env?: NodeJS.ProcessEnv }) => boolean;
  /** Sync spawn that returns {status, stdout, stderr, resolved, notFound}. */
  runCommand: (command: string, args?: string[], options?: Record<string, unknown>) => {
    status: number | null; stdout: string; stderr: string; resolved: string; notFound: boolean;
  };
  /** Sync npm run; returns stdout. Throws ENOENT when npm is missing. */
  runNpm: (args?: string[], options?: Record<string, unknown>) => string;
  /** Sync CLI login probe; loginArgs like ['login', 'status']. */
  probeCli: (command: string, loginArgs?: string[]) => { available: boolean; mode: string; reason: string };
  /** Spawn a CLI adapter with guardrails, watchdog, and change-footer handling. */
  runCli: (context: RunContext, adapter: CliAdapter) => Promise<RunResult>;
  /** Git working-tree fingerprint for a project directory. */
  gitState: (cwd: string) => { dirty: boolean; snapshot: string; changed: string };
  /** JSONL event stream parser; interpret returns {sessionId?, activity?}. */
  jsonLineEvents: (
    interpret: (event: Record<string, unknown>, state: Record<string, unknown>) => { sessionId?: string; activity?: string } | void,
    onProgress?: (text: string) => void,
    onSession?: (sessionId: string) => void,
  ) => CliEventStream;
  /** Update state from assistant/result events (Claude-style stream-json). */
  assistantEvent: (event: Record<string, unknown>, state: Record<string, unknown>, options?: { append?: boolean }) => void;
  /** Categorical activity text for Claude/Qoder-style stream events. */
  streamActivity: (event: Record<string, unknown>) => string;
  /** Categorical progress text for a tool name. */
  toolActivity: (name: string, completed?: boolean) => string;
  /** Concatenated text of string-or-blocks content. */
  textBlocks: (content: unknown) => string;
}
export interface PluginConfig {
  locale?: 'zh-CN' | 'en';
  stateDir: string;
  models?: Record<string, string>;
  /** Values for one plugin. A packaged plugin declares the keys in plugin.json; the bridge fills this from that package's settings.json. */
  pluginSettings?: Record<string, Record<string, string>>;
  /** Cross-platform helpers; prefer these over hand-rolled spawn/npm calls. */
  runtime?: PluginRuntime;
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
  /** Declare that progress/error/reason strings are bridge-canonical messages; clients localize them at output boundaries. */
  bridgeStrings?: boolean;
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
/** Bridge handles for board plugins: what an enhancement can reach without a hook. */
export interface BridgeFacade {
  /** Enabled coding agents with a probed availability snapshot. */
  agents(): { id: string; label: string; available: boolean; reason: string; mode: string; capabilities: Capabilities }[];
  agent(id: string): ReturnType<BridgeFacade['agents']>[number] | undefined;
  /** Persisted jobs, newest first. */
  jobs(): Record<string, unknown>[];
  job(id: string): Record<string, unknown> | undefined;
  sessions(agentId?: string): Record<string, unknown>[];
  /** Create a task (waiting_confirmation) for an agent; rate-limited per bridge. */
  createTask(options: { instruction: string; agentId?: string; projectId?: string; title?: string }): { job: Record<string, unknown>; session: Record<string, unknown> };
  /** Append a turn to an existing session. */
  submitTurn(options: { instruction: string; sessionId: string }): Record<string, unknown>;
  confirmJob(id: string): Record<string, unknown>;
  cancelJob(id: string): Record<string, unknown>;
  themes(): { name: string; title: string; sound: boolean; icon: boolean; background: boolean }[];
  devices(): string[];
  /** Action system: command names derived from the device command catalog (`action.<name>` entries). Two-level binding model (docs/SPRITES.md): sprites declaring a `commands` map expose abstract slots `action.A1`–`action.A9` ("command binds artwork", decided by the sprite author); legacy sprites expose semantic action names (deprecated). Advisory only — the board silently ignores unknown action commands. */
  deviceActions(deviceId: string): string[];
  /** Enqueue a board command directly; only the board-side catalog constrains the name. */
  send(deviceId: string, command: BoardCommand, audio?: Uint8Array): QueuedBoardCommand;
  pending(deviceId: string): QueuedBoardCommand[];
}
/** Turn input pipeline: return a new string to override what the agent executes. */
export interface TurnInputContext {
  instruction: string;
  job: Job;
  agentId: string;
  config: PluginConfig;
  bridge: BridgeFacade;
}
/** Turn output pipeline: called only when a job completes; a returned string overrides the result. */
export interface TurnOutputContext {
  result: string;
  job: Job;
  agentId: string;
  status: 'completed';
  config: PluginConfig;
  bridge: BridgeFacade;
}
export type JobEventType = 'created' | 'confirmed' | 'started' | 'progress' | 'completed' | 'handed_off' | 'failed' | 'cancelled';
/** Job lifecycle notification; fire-and-forget, failures never affect the job. */
export interface JobEventContext {
  type: JobEventType;
  job: Job;
  config: PluginConfig;
  bridge: BridgeFacade;
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
    bridge: BridgeFacade;
  }) => Promise<void> | void;
  onEvent: (context: {
    event: BoardEvent;
    config: PluginConfig;
    deviceId: string;
    send: (command: BoardCommand, audio?: Uint8Array) => QueuedBoardCommand;
    bridge: BridgeFacade;
  }) => Promise<{ commands?: BoardCommand[] } | void> | { commands?: BoardCommand[] } | void;
  /** Turn input pipeline: a returned string overrides the instruction actually executed (the stored original stays intact). */
  onTurnInput?: (context: TurnInputContext) => string | void | Promise<string | void>;
  /** Turn output pipeline: called only for completed jobs; a returned string overrides the result. */
  onTurnOutput?: (context: TurnOutputContext) => string | void | Promise<string | void>;
  /** Job lifecycle notification. Tasks created by plugins do not broadcast 'created'. */
  onJob?: (context: JobEventContext) => void;
}
export type VibePlugin = CodingAgentPlugin | BoardPlugin;
