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

## Bridge side

Handles exposed by `bridge/src/plugins/enhance.mjs`; take effect after restarting the bridge:

- Trigger surface: `onEvent` / `onConnect` / `onDisconnect` (board events and connections), `onJob` (task lifecycle: `created` / `confirmed` / `started` / `progress` / `completed` / `handed_off` / `failed` / `cancelled`; tasks created by plugins themselves do not broadcast `created`, to prevent self-excitation), `onTurnInput` / `onTurnOutput` (turn pipeline: chained in registration order, returning a string overrides, last writer wins; 1500 ms timeout, a failing plugin falls through to the previous link and never blocks a task; `onTurnInput` mutates the execution copy, the task's original text is untouched).
- The `bridge` facade (available in event, hook and notification contexts): `agents()` / `agent(id)` / `jobs()` / `job(id)` / `sessions(agentId?)` / `createTask({instruction, agentId?, projectId?, title?})` (creates a session and lands a pending task automatically, rate-limited to 10/min) / `submitTurn({instruction, sessionId})` / `confirmJob(id)` / `cancelJob(id)` / `themes()` / `devices()` / `send(deviceId, command, audio?)` / `pending(deviceId)`.

The interface type source of truth is `bridge/src/plugins/contracts.d.ts`; usage and full examples are in [docs/PLUGINS.md](../PLUGINS.md). The rule for assigning a capability to a line: does the board have this command/event (firmware's job), or does the bridge have this handle in memory (bridge's job) — the former means flashing firmware, the latter means editing the bridge; plugins are always just consumers of the list.

## Assistant avatar assets

An agent plugin package may optionally carry a `sprite/` directory (layered PNGs + manifest), delivered with the package to made's near-full-screen sprite — changing avatars never requires a firmware flash. For format, animation semantics and the chunked fetch protocol, see [docs/SPRITES.md](../SPRITES.md); the scaffold `npx made-market init --kind agent` ships a template, and any asset change must bump the manifest `version`.
