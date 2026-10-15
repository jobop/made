<div align="center">

# made · 码得

**An open, extensible AI companion — the electronic pet of the AI era.**

> 一个开源、可扩展的 AI 伴侣，AI 时代的电子宠物。
> Speak a task. Watch it run. Any agent can get its own hardware with a single plugin.

[English](README.md) · [简体中文](README.zh-CN.md)

![License](https://img.shields.io/badge/license-MIT-green) ![Hardware](https://img.shields.io/badge/hardware-ESP32--S3-orange) ![Bridge](https://img.shields.io/badge/bridge-Node%20%E2%89%A5%2022-blue) ![Firmware](https://img.shields.io/badge/firmware-ESP--IDF%205.5-red)

**Made, when you want to make.** · **码得，代码触手可得**

</div>

---

## 🎯 Overview

**made** is an open-source hardware platform that brings AI agents into the physical world. It transforms your desk into a thinking space: a palm-sized device with a round capacitive touch display and a layered animated sprite that breathes, blinks, and talks while your agent works.

**码得** is the open-source hardware platform that brings AI agents into the physical world. It transforms your desk into a thinking space: a palm-sized device with a round capacitive touch display and a layered animated sprite that breathes, blinks, and talks while your agent works.

### The AI Era Electronic Pet

It is an **electronic pet for the AI era**: not a single-purpose gadget, but open, hackable hardware that grows through plugins. An agent is not a feature we shipped — it is a plugin you write. Bring your own agent, and it gets its own face, voice channel, session memory, and a physical body on your desk.

### Why made?

Coding agents live in terminals and IDEs — invisible, silent, far away. **made** puts one on your desk.

- **🐾 A pet, not a remote control.** The sprite idles, breathes, blinks and lip-syncs; task states drive its motion. Themes and sprites ship as plugin packages — no firmware reflashing.
- **🔌 One plugin = one hardware resident.** Assistant names, icons, capabilities and full-screen sprites are all delivered by the bridge at runtime. A new agent never touches the firmware.
- **🛒 An ecosystem, not a walled garden.** A plugin marketplace (`npx made-market`) distributes agent runtimes, control plugins and themes, plus expert packs for personalities. Users extend their device; developers reach theirs.
- **🎙 Voice-first interaction.** Record → transcribe → review on screen → confirm with a double-press. Nothing runs until you confirm it.
- **🔗 Four ways to connect.** USB cable, LAN Wi-Fi, a second-board receiver hotspot, or a public HTTPS tunnel — same protocol, same device API.
- **🔓 Fully open.** MIT licensed: ESP-IDF firmware, a Node.js bridge, a web workbench, 3D-printable case files. No cloud account required.

## 🧠 Architecture

```
        voice / buttons                  device API (event ⇄ command queue)
  you ───────────────▶ ┌─────────┐ ─────────────────────────▶ ┌──────────┐
                       │  made   │      USB · Wi-Fi · LAN     │  bridge  │
  you ◀─────────────── │  device │      receiver · public     │          │
        sprite screen  └─────────┘        hotspot             │          │
                                                                       │ CLI sessions
                                                          ┌────────────┼────────────┐
                                                          ▼            ▼            ▼
                                                        Codex       Cursor      your plugin…
```

The **device firmware** is a generic terminal: it records audio, renders assistant sprites and replies, and exchanges a small event/command protocol. The **bridge** runs on your computer: it hosts the web workbench (`127.0.0.1:8787`), serves the device API (`8788`), runs agent plugins as CLI sessions in your Git repositories, and relays progress, confirmation and cancellation back to the device. A **task is a session**: create it once, keep talking in the same context.

## 💻 Hardware

| Feature | Specification |
| --- | --- |
| Reference board | Waveshare `ESP32-S3-Touch-LCD-1.85B` |
| Display | 360 × 360 round capacitive touch |
| MCU / Memory | ESP32-S3 · 16 MB flash · 8 MB PSRAM |
| Optional receiver | A second ESP32-S3 or ESP32-C3 board as a Wi-Fi→USB relay for restricted networks |
| Enclosure | 3D-printable case in [`3d-model/`](3d-model/) (sources, STL, 3MF) |

The product and protocol are display-shape agnostic — the round screen is just the current implementation.

### 3D-printed case

The case is fully open and parametric — no CAD skill required:

- **Ready to print:** [`made_case_plate.3mf`](3d-model/made_case_plate.3mf) is a pre-arranged print plate (Bambu Studio or any slicer that reads 3MF). Individual parts are also shipped as STL: [`made_case_front.stl`](3d-model/made_case_front.stl), [`made_case_back.stl`](3d-model/made_case_back.stl), [`made_case_buttons.stl`](3d-model/made_case_buttons.stl).
- **Labels & preview:** [`PARTS.md`](3d-model/PARTS.md) is the part dictionary, [`PARTS-diagram.svg`](3d-model/PARTS-diagram.svg) the labeled diagram, and `preview.png` the rendered result.
- **Regenerate:** the whole shell comes from one parametric script — `blender -b -P 3d-model/make_case.py` rebuilds all three STLs. Baseline board outline is 50.0 × 85.8 mm; tweak the constants at the top of the script to fit a different board.

## 🚀 Quick start

**1. Run the bridge** (Node.js ≥ 22, no npm dependencies):

```sh
cp bridge/config.example.json bridge/config.local.json
npm start
```

Open the workbench at [http://127.0.0.1:8787](http://127.0.0.1:8787) and point `projects` in `bridge/config.local.json` at your Git repositories. `bridge/demo-project` is a safe sandbox to try first.

**2. Flash the device** with one command:

```sh
npm run flash          # scan → identify board → pick target → flash
```

See [docs/FLASH.md](docs/FLASH.md) for targets, board identification and prebuilt images.

**3. Connect** — swipe up on the lock screen, then choose whichever fits: USB cable, LAN auto-discovery, a manual/public HTTPS address, or the optional receiver. Full walkthroughs in [docs/CONNECTIONS.md](docs/CONNECTIONS.md).

**4. Install agents** from the marketplace:

```sh
npx made-market install codex cursor qoder workbuddy
```

**5. Configure voice** — any OpenAI-compatible transcription endpoint works (URL + API key + model, e.g. OpenAI or SiliconFlow SenseVoice). Details in [docs/VOICE.md](docs/VOICE.md).

**On the device:** swipe left/right to switch assistants · `+` creates a task · BOOT once records, twice confirms, three times cancels, hold ~1.2 s returns to the desktop. The device only talks to the bridge while the app is in the foreground and unlocked.

## 🧪 Write your own agent — one plugin away

An agent is a JavaScript object. Save it under `~/made/plugins/`, restart the bridge, and it appears on the device — name, icon, sprite, capabilities and all:

```js
export default {
  apiVersion: 1,
  kind: 'coding-agent',
  id: 'my-agent',                     // stable ID — sessions hang off it
  label: 'My Agent',
  capabilities: { session: 'native', model: true, cancel: true, progress: true },
  probe: () => ({ available: true }),
  async run({ job, project, signal, model, onProgress, onSession }) {
    onProgress('thinking…');
    onSession('my-native-session-id');  // bridge persists it; next turn passes job.sessionId back
    // do the actual work with job.instruction inside project.path; honor signal
    return { status: 'completed', result: 'Done.' };
  },
};
```

Beyond coding agents, **control plugins** subscribe to device events (`boot.click`…), rewrite turns through the `onTurnInput`/`onTurnOutput` pipelines, create tasks, and send commands back to the device (`caption.show`, `audio.play`, `theme.apply`…). **Theme plugins** restyle the pet; **sprite packages** give each agent its full-screen character.

- Plugin development guide: [docs/PLUGINS.md](docs/PLUGINS.md)
- Sprite (layered character art) spec: [docs/SPRITES.md](docs/SPRITES.md)
- Full bridge capabilities table: [AGENTS.md](AGENTS.md)

## 🛍️ Plugin ecosystem

Three markets, each owning one layer — packages only install and register; the bridge executes:

| Market | What it manages | Effect |
| --- | --- | --- |
| **Plugin market** | The device itself: themes, control plugins, agent runtimes | Installed into the bridge (`~/made/{plugins,themes}`); on-device resources delivered over the air |
| **Skills** | Reusable agent abilities | Reuses the existing skills marketplace; attached per agent |
| **Expert market** | Personalities injected into agents | Installed into the bridge and bound to an agent; injected on every run |

Marketplace CLI:

```sh
npx made-market init <dir> --kind agent     # scaffold (agent / control / theme)
npx made-market validate <dir>              # same checks as the server
npx made-market publish <dir>               # publish (login required)
npx made-market install <package-id>        # install into the bridge
npx made-market info <package-id>           # versions, checksums, install location
```

Marketplace boundaries and classification rules: [docs/MARKETS.md](docs/MARKETS.md).

## 📁 Repository layout

```
made/               Device firmware (ESP-IDF 5.5, Brookesia desktop, made app)
receiver/           Optional ESP32-S3 / ESP32-C3 receiver firmware
bridge/             Computer bridge: workbench UI, device API, agent runtime, examples
docs/               Guides: plugins, markets, sprites, connections, voice, flashing…
plugin-market/      Plugin package registry (pack management only)
expert-market/      Expert (personality) package registry (pack management only)
3d-model/           3D-printable case (sources, STL, 3MF)
sim/                Host simulator for the device UI (npm run sim)
tools/              Flash tool and dev utilities
```

## 📖 Documentation

| Document | Contents |
| --- | --- |
| [docs/PLUGINS.md](docs/PLUGINS.md) | Writing agent & control plugins: contracts, sessions, icons, turn pipelines |
| [docs/MARKETS.md](docs/MARKETS.md) | What belongs in which market; packaging rules |
| [docs/SPRITES.md](docs/SPRITES.md) | Layered sprite art spec: manifest schema, motion semantics, size limits |
| [docs/CONNECTIONS.md](docs/CONNECTIONS.md) | All connection modes: USB, LAN, receiver, public tunnels, phone provisioning, pairing security |
| [docs/VOICE.md](docs/VOICE.md) | Speech recognition setup: compatible endpoints, presets, local Whisper |
| [docs/FLASH.md](docs/FLASH.md) | Flash tool usage, board identification, write-address tables |
| [docs/PLATFORMS.md](docs/PLATFORMS.md) | macOS / Linux / Windows capability matrix (`npm run doctor` self-check) |
| [docs/I18N.md](docs/I18N.md) | zh-CN / en localization boundaries and dictionaries |
| [made/](made/README.md) · [receiver/](receiver/README.md) | Firmware build & porting notes |

> Deep technical documents are currently written in Chinese; the README is bilingual.

## 🧪 Testing

```sh
npm test          # bridge-side suite: jobs, sessions, pairing, USB framing, voice upload…
npm run sim       # host simulator for the device UI (screenshots without hardware)
npm run doctor    # environment self-check for your machine
```

## 🤝 Contributing

Issues and pull requests are welcome. For plugin ideas, start with [docs/PLUGINS.md](docs/PLUGINS.md) and the `npx made-market init` scaffold — most extensions need no firmware work. Firmware changes should keep all four connection paths working (see [AGENTS.md](AGENTS.md)).

## 📄 License

[MIT](LICENSE)
