# Plugin Open-Capability List

> Since 2026-10, the bridge no longer bundles coding assistants: codex / cursor / qoder / workbuddy are now pluginized as marketplace packages (source of truth `~/WorkBuddy/made-market-plugins/`); reinstalling a package restores it. The CLI execution mechanism stays in `bridge/src/agents/` and is injected into plugins via `config.runtime`.

A governance plugin's capability surface = the union of two supply lines. Check this table before writing plugins or reviewing requirements; the marketplace site mirrors it on the [open-capabilities page](https://made-market.link/capabilities.html), and the scaffold `npx made-market init --kind control` generates the same list into the package (`capabilities.json` + `contracts.d.ts`). **When adding a new plugin capability, three places must be updated in sync: this list, the marketplace CLI scaffold (made-market repo `cli/`), and the marketplace site page (`site/capabilities.html`).**

## Board side

The list is reported by the firmware (`announceBoardCatalog` in `esp_brookesia_app_vibe.cpp`); extending it requires a firmware flash.

| Events (cadence is always edge) | Trigger |
| --- | --- |
| `boot.click` | Button short press (voice intent) |
| `boot.double` | Double press (confirm) |
| `boot.triple` | Triple press (cancel) |
| `boot.long` | Long press (back to home) |
| `motion.shake` | Shake gesture detected by the on-board IMU (QMI8658, `vibe_motion` component; ≥3 accel spikes within 700 ms, 2.5 s cooldown). Field `agent` = assistant id whose sprite page is current — the firmware tracks it in memory at page-switch time, no heartbeat delay. The event only fires while some board plugin subscribes it. Boards without an IMU never report it (verify with the firmware I2C bus scan) |
| `touch.double` | Two short taps on the assistant page within 450 ms (empty screen areas only; taps on buttons/drawer/dialogs don't bubble to the screen). Field `agent` = current assistant id, same as `motion.shake`. Introduced as the no-IMU substitute trigger for fall-style reactions; fires only while subscribed |

| Command | Purpose |
| --- | --- |
| `caption.show` | On-screen caption (field `text`, plus duration etc.; see firmware parsing) |
| `audio.play` | Play audio, optionally with a WAV attachment (16-bit PCM, 8k–48 kHz, ≤256 KB) |
| `theme.apply` | Apply a theme plugin |
| `volume.set` | Volume |
| `agent.select` | Switch the active assistant |
| `session.create` / `session.select` / `session.delete` | Session management |
| `voice.start` | Actively light up recording |
| `task.confirm` / `task.cancel` | Approve / reject a pending task |
| `ui.swipe` | Semantic gestures: on the home screen `direction` = `left`/`right` (switch assistant), `up` (open drawer), `down` (close drawer); inside the drawer (task-result page) `left`/`right` switches between that assistant's tasks |
| `ui.tap` | Tap the screen: fields `x`/`y` (panel pixel coordinates); hit-tests the deepest clickable object and dispatches CLICKED |
| `ui.back` | Remote back key: exits layer by layer along the same path as the physical back key (settings panel / drawer / dialogs); still works while the settings page is open (ui.swipe/ui.tap are intercepted there) |
| `debug.screenshot` | Capture the current screen; RGB565 is returned to the bridge and converted to PNG (see [On-device screenshot debugging](04-build-and-flash.md#on-device-screenshot-debugging)) |
| `action.<name>` | Trigger a sprite action: one catalog entry per triggerable command (`action.A1`… for sprites declaring a `commands` map; legacy `action.fall`-style semantic names for old sprites, deprecated). Unknown/mapped-missing commands are silently ignored — degradation, never an error. Legacy form `action.play` with field `action` is still accepted |

### Device action entries (action system, two-level binding)

Two-level binding model (see [docs/SPRITES.md](../SPRITES.md) "跨角色平台契约"): **event → command** is owned by the enhancement plugin (made-kit) via its settings — board events (`touch.double`, `motion.shake`) map to abstract command slots `A1`–`A9` (opaque numbers, no agreed semantics), each binding individually switchable. **Command → artwork** is owned by the sprite author via the manifest `commands` table (`{"A1": "fall"}` — slots map to actions declared in the sprite's own `actions` table). The two sides never see each other.

The device owns its trigger list: sprites declaring a `commands` map expose abstract slots (`action.A1`…); the device resolves the slot through the map at trigger time. Sprites without a `commands` map expose their semantic action names directly (legacy, deprecated). Either way the firmware folds the entries into the command catalog and re-announces the whole catalog (`POST /device/capabilities`) whenever the sprite changes the list. The bridge derives the names from the catalog and exposes them as `deviceActions` in `/api/state` and via the facade `deviceActions(deviceId)`; the list is advisory only (the board silently ignores unknown `action.*` commands).

Two action forms share this surface: track-table actions (per-layer frame sequences) and whole-frame GIF actions (`{"gif": "x.gif", "ms": 3000}` — the firmware streams a built-in minimal GIF decoder onto a full-canvas overlay and hides the layered sprite for the duration). Asset-side constraints (canvas ≤512², ≤256 KB/file, ≤10 fps, per-frame transparency) and authoring recipes live in `docs/SPRITES.md` and the made-kit skill pack (`check_gif.py`, `action_gif.py`, the action-plugin template).

**Cross-role contract**: sprite authors and board-plugin authors are usually different people, so the platform decouples them with the two-level binding above. Plugin authors only emit abstract slots (`bridge.send(deviceId, { name: 'action.A1' })`) and must tolerate the slot being unmapped (silent degradation, no error/retry storms); sprite authors decide what each slot plays by writing the `commands` table. Reuse made-kit's built-in event bindings (double-tap→A1, shake→A2) before writing a new trigger plugin. Action names inside the `actions` table are package-internal organization (lowercase English recommended: `fall`, `dance`, `yawn`…); frame files follow `<layer>-<action>-<n>.png` and GIFs `<action>.gif` — file and action names are never part of the cross-package contract.

## Bridge side

Handles exposed by `bridge/src/plugins/enhance.mjs`. Saving a plugin, or installing / enabling / disabling one, hot-reloads them in the running bridge; a process restart still loads them too:

- Trigger surface: `onEvent` / `onConnect` / `onDisconnect` (board events and connections), `onJob` (task lifecycle: `created` / `confirmed` / `started` / `progress` / `completed` / `handed_off` / `failed` / `cancelled`; tasks created by plugins themselves do not broadcast `created`, to prevent self-excitation), `onTurnInput` / `onTurnOutput` (turn pipeline: chained in registration order, returning a string overrides, last writer wins; 1500 ms timeout, a failing plugin falls through to the previous link and never blocks a task; `onTurnInput` mutates the execution copy, the task's original text is untouched).
- The `bridge` facade (available in event, hook and notification contexts): `agents()` / `agent(id)` / `jobs()` / `job(id)` / `sessions(agentId?)` / `createTask({instruction, agentId?, projectId?, title?})` (creates a session and lands a pending task automatically, rate-limited to 10/min) / `submitTurn({instruction, sessionId})` / `confirmJob(id)` / `cancelJob(id)` / `themes()` / `devices()` / `deviceActions(deviceId)` (action names derived from the device catalog's `action.*` entries) / `send(deviceId, command, audio?)` / `pending(deviceId)`.

The interface type source of truth is `bridge/src/plugins/contracts.d.ts`; usage and full examples are in [docs/PLUGINS.md](../PLUGINS.md). The rule for assigning a capability to a line: does the board have this command/event (firmware's job), or does the bridge have this handle in memory (bridge's job) — the former means flashing firmware, the latter means editing the bridge; plugins are always just consumers of the list.

## Assistant avatar assets

An agent plugin package may optionally carry a `sprite/` directory (layered PNGs + manifest), delivered with the package to made's near-full-screen sprite — changing avatars never requires a firmware flash. For format, animation semantics and the chunked fetch protocol, see [docs/SPRITES.md](../SPRITES.md); the scaffold `npx made-market init --kind agent` ships a template, and any asset change must bump the manifest `version`.
