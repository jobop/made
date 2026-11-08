# 08: Fix the generator, not just the instance

When a bug is found in a plugin or in the bridge, fixing the running instance is
never the whole job. Every fix must be pushed back to the things that **create**
plugins and bridges, so the same bug does not come back with the next generated
assistant or the next deployment.

## The rule

Fixing an issue in any of these layers requires fixing **all** layers that can
reproduce it:

| Layer | Source of truth | Must also update |
| --- | --- | --- |
| An installed plugin (e.g. `cat-mimi`, `made-ling`) | `made-market-plugins/<pkg>/` repo | `made-kit` (templates, guides, `check_package.py` guards) — it generates future assistants |
| The bridge (behavior, endpoints, protocol) | `made/bridge/src/` repo | `made-kit` docs/templates that describe the contract; `made-cloud/e2b-template/` if the template pre-installs or configures it |
| A generated assistant's config/models | The generated package repo | `made-kit/skill/guides/agent-plugin.md` rules + static checks |

## Why

- 2026-11-07: `made-ling` shipped a private `models.json` whose entries lacked
  `input: ['text', 'image']` — images were silently dropped. The four-layer fix
  (plugin, market, template, sandbox) was applied, but **made-kit was missed**;
  the generator would have produced the same bug in the next assistant.
  Caught by the user; fixed in made-kit 1.0.69 (guide rule + `check_package.py`
  static guard).
- Instance-only fixes rot: sandboxes are rebuilt, packages reinstalled, but
  generated assistants keep coming from made-kit and the bridge keeps deploying
  from `made/bridge/src/`.

## Checklist before calling a plugin/bridge fix "done"

1. Instance hot-fixed (deployed sandbox / running bridge) — for immediate relief.
2. Source repo fixed and version-bumped (`market.json` / `package.json`), published
   via `npx made-market publish` / `npm publish` as applicable.
3. **made-kit updated**: if the bug is one a generated assistant could repeat,
   add/adjust the rule in `skill/guides/*.md` and, where mechanically detectable,
   a guard in `skill/tools/check_package.py` (guards need a bad-sample test that
   fails, plus existing good packages passing).
4. **Bridge source updated** in `made/bridge/src/` if the bridge's own behavior
   changed — never patch only the installed copy under `~/made/plugins/` or a
   deployed sandbox.
5. Cloud template synced (`made-cloud/e2b-template/agent-plugins/`) if the template
   pre-installs the package.
6. Verified on at least two instances (see `06-no-overfitting.md`).
