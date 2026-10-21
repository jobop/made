# AGENTS.md

This file is the development-standards index. The standards themselves are split by topic under `docs/standards/` — **read the relevant entry before touching code**. Detailed reference docs live in `docs/` (platform support, markets, plugins, flashing, voice, sprite assets, i18n, connections).

## Development standards (docs/standards/)

| Document | Contents |
| --- | --- |
| [Connection coverage rule](docs/standards/01-connection-coverage.md) | Every new feature and interface must support all four connection paths: USB cable / LAN Wi-Fi / receiver relay / public endpoint. Platform differences in `docs/PLATFORMS.md`; run `npm run doctor` for a local verdict |
| [Marketplace capability split](docs/standards/02-market-capabilities.md) | The three marketplaces (plugins / skills / experts) each own one layer and do package management only; installing and publishing must go through the `npx made-market` CLI (hard rule, with two easy pitfalls) |
| [Plugin open-capability list](docs/standards/03-plugin-capabilities.md) | The full capability surface across the board side (firmware-reported event/command tables) and the bridge side (enhance.mjs handles and the `bridge` facade); new capabilities must be synced in three places |
| [Startup & flashing](docs/standards/04-build-and-flash.md) | The `npm run flash` tool, starting the bridge (including the sandbox proxy-env trap), task lanes, stuck-job self-healing, board identification, manual flashing of made/receiver, on-device screenshot debugging |
| [Case model (3d-model)](docs/standards/05-case-model.md) | The case is generated entirely by the parametric `3d-model/make_case.py`; consult the parts dictionary before any change. Trigger phrase "3d 外壳标注图" = open `3d-model/PARTS-diagram.svg` |
| [No overfitting to a single sample](docs/standards/06-no-overfitting.md) | Tools, templates and docs must work for arbitrary new instances without code changes: no hardcoded character/asset/path assumptions, per-instance input resolution, two different instances before claiming generality, auto-resolved inputs must print their pick and accept an override |

## Reference docs (docs/)

- [Platform support](docs/PLATFORMS.md) — per-OS availability and limits of each connection path
- [Marketplace split](docs/MARKETS.md) — marketplace classification, boundaries and mixing rules
- [Plugin development](docs/PLUGINS.md) — plugin usage and full examples
- [Flash tool](docs/FLASH.md) — targets, board-ID rules, options, flash address table and troubleshooting
- [Sprite assets](docs/SPRITES.md) — assistant avatar format, animation semantics and chunked fetch protocol
- [Voice](docs/VOICE.md) / [Connections](docs/CONNECTIONS.md) / [I18N](docs/I18N.md)
