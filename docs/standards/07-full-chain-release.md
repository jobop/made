# 07 — Full-chain release (全链路发布)

Trigger phrase: **「全链路发布」**. Run all five steps below, in order. Do not skip steps and do not reorder: the market publish and the board flash both depend on the committed/merged state, and the deploy depends on the local source being the source of truth.

## 1. Commit & push

- Commit every pending change with a descriptive message, then push the working branch (`git push origin <branch>`).
- `made-market-plugins` is a separate git repo (`~/My project/made-market-plugins`); commit and push its pending plugin changes in the same step.
- Pitfall: this repo's HTTPS push to GitHub may fail with `Error in the HTTP2 framing layer`, and the local proxies (127.0.0.1:7890/7892) are often down. Fallback order: `git -c http.version=HTTP/1.1 push`, then proxy, then push to the SSH URL `git@github.com:jobop/<repo>.git` (SSH is the reliable path).

## 2. Merge to main

- `git checkout main && git merge <branch> --ff-only && git push origin main`. Use `--ff-only`; a failed fast-forward means the branch needs a rebase first, not a merge commit.

## 3. Deploy made-cloud (server 47.90.84.89)

- The made-cloud source of truth is the local dir `/Users/myidd007/My project/made-cloud` (not a git repo). Deploy over SSH as root (key auth, `BatchMode`):
  `tar czf - src package.json | ssh root@47.90.84.89 'tar xzf - -C /opt/made-cloud && systemctl restart made-cloud'`
  (macOS tar emits harmless `LIBARCHIVE.xattr` warnings on the server.)
- Never ship local `node_modules/`, `data/` or `e2b-template/` — the server has its own (runtime data + the real e2b SDK).
- `package.json` must declare the e2b version that the server actually runs (`^2.62.0`). The local `node_modules/e2b` is a **test stub** (`0.0.0-mock-stub`), so local deps are irrelevant to the deploy.
- Verify: `systemctl is-active made-cloud` → `active`, and the public endpoint answers (`curl https://cloud.made-world.link/` returning 401 within ~1s is healthy; DNS is grey-cloud/直连).

## 4. Publish marketplace packages

- Only via `npx made-market validate <id>` then `npx made-market publish <id>`, run from `~/My project/made-market-plugins` (`unset NODE_OPTIONS PYTHONPATH` first). Server: market.made-world.link, credentials in `~/made/credentials.json`.
- `publish` reporting 「版本已存在」 is usually not an error: confirm with `npx made-market info <id>` — if the server already shows the same version, the package is already live and the step is done.
- `fetch failed` may just be slow, not unreachable; retry `info` before assuming breakage.

## 5. Flash the board

- Kill every bridge process that could steal the serial port first (a leftover bridge auto-reconnects and makes esptool report `chip stopped responding`):
  `lsof -t /dev/cu.usbmodem*` → kill those pids, and kill any node process whose cwd is a `bridge/` dir; then confirm `lsof /dev/cu.usbmodem*` is empty.
- Flash: `unset PYTHONPATH NODE_OPTIONS && export PATH="/usr/bin:$PATH" && source $HOME/esp/esp-idf/export.sh && npm run flash -- made --build -y` (docs/FLASH.md). The `--build` flag is mandatory — without it a stale binary gets flashed.
- Verify the firmware binary was actually rebuilt: check `made/build-s3/01_factory.bin` mtime is fresh.

## Done criteria

All five steps executed with their verifications green: pushes visible on origin, main up to date, service `active` and public endpoint answering, market versions confirmed via `info`, board flashed from a freshly built binary.
