# Startup & Flashing

The directories are `made/` (made firmware), `receiver/` (receiver firmware), `bridge/` (bridge). Never run `node src/server.mjs` from the repo root, and never incrementally build in the old build directories still pointing at `firmware-brookesia` or `receiver-firmware`.

## Flash tool

For day-to-day flashing use the flash tool `tools/flash.mjs`: one command to identify the board, pick a target and flash, defaulting to no full-flash erase so NVS survives. It codifies the manual flow below:

```sh
# Stop the bridge occupying the serial port first, then flash
npm run flash                              # interactive: scan → board ID → suggested target → confirm
npm run flash -- --scan                    # board identification only
npm run flash -- made -p /dev/cu.usbmodem101 -y
npm run flash -- receiver-c3-usb --build   # build first, then flash
```

For targets, board-ID and recommendation rules, all options, the flash address table and troubleshooting, see the [Flash tool](../FLASH.md). The manual flow below is for fine-grained control (app-partition-only writes, custom `sdkconfig`).

## Starting the bridge

Before flashing or restarting, stop any old processes holding ports `8787`, `8788` and `/dev/cu.usbmodem*`. A bridge left over from before a directory rename keeps holding the serial port, and esptool then cannot reach the board.

Run from the repo root:

```sh
npm start
```

This descends into `bridge/` and runs `node src/server.mjs`. The config file is `bridge/config.local.json`; copy it from `bridge/config.example.json` if missing. The bridge is also published to npm (package `made-bridge`): on any machine, `npx made-bridge` starts it in one command; on first run it generates config and state under `~/.made-bridge/` (the code directory is read-only; all writable data lives in the user directory; `VIBE_CONFIG` / `VIBE_STATE_DIR` / `VIBE_DATA_DIR` can override). The workbench is `http://127.0.0.1:8787`, the device API is `8788`, LAN discovery is UDP `8789`. Tests also run from the repo root: `npm test`.

### Startup environment: never launch the bridge from a sandboxed session

WorkBuddy / sandboxed shells inject a **session-local proxy**
(`HTTP_PROXY=HTTPS_PROXY=http://127.0.0.1:<random port>`) plus a pile of shim variables into the
environment, and the bridge forwards them **verbatim to child CLIs** via
`spawn(..., { env: process.env })`. codex is Rust reqwest and honors
`HTTPS_PROXY`: after sending every request to a long-gone port it **retries forever**
(process alive, 0% CPU, zero sockets) and the task stays stuck at "analyzing task" forever. Node-based
assistants ignore the variable so they themselves are fine, but they queue behind the stuck lane —
it looks like "all assistants are stuck".

When launching the bridge from a session, first strip `HTTP_PROXY`/`HTTPS_PROXY`/`ALL_PROXY`/`NO_PROXY`
(all casings), `NODE_OPTIONS`, `PYTHONPATH`, `BASH_ENV` and `CODEBUDDY_*`.
Debugging mantra: `ps eww -p <bridge PID>` to check its environment for proxies; if
`lsof -a -p <CLI PID> -i` is empty, it is not waiting on the network.

### Task lanes

`pump()` in `bridge/src/jobs.mjs` lanes tasks **by session**: sessions may run in parallel, but tasks
within one session are strictly serial (coding tools take a per-session thread write lock; parallel
runs in one session always conflict, and `submit()` directly rejects a second concurrent
instruction for the same session). Default concurrency is 2 sessions; tune with
`maxParallelJobs` in `bridge/config.local.json` or the `VIBE_MAX_PARALLEL` env var, range 1–4.

### Stuck-job self-healing

A coding-tool process may go silent for a long time (network death, hung model endpoint — process
alive, zero progress). `runCli` in `bridge/src/agents/shared.mjs` has a watchdog: if a task produces
no output for `stuckIdleMinutes` (default 5; `VIBE_STUCK_IDLE_MINUTES`, range 1–60) straight, the
whole process tree is killed and the error is marked retryable; the `JobStore` lane then re-runs the
same instruction automatically, up to `maxAutoRetries` (default 1; `VIBE_MAX_AUTO_RETRIES`, 0–3)
times — native sessions resume the same CLI thread. If the re-run stalls too, the task returns to
"pending confirmation" with the reason stated, and the user can confirm manually to retry. The
20-minute hard timeout is handled the same way. So "bridge restart recovery + watchdog kill +
automatic re-run + manual fallback" forms a complete self-healing chain — a stuck task no longer
requires restarting the bridge to unlock.

made does not connect to the bridge or report heartbeats while locked. After a flash-reboot, the
workbench showing the device offline is expected; it comes back online after swipe-to-unlock.

## Identify the board before flashing

Both boards can enumerate as Espressif `303a:1001`. Run `python -m esptool --port /dev/cu.usbmodemXXXX chip_id` on every port first, then decide the image. Ports change with which USB port you use.

- ESP32-S3 with 8 MB PSRAM: made (Waveshare 1.85B), 16 MB flash, only images from `made/`.
- ESP32-C3: receiver, 4 MB flash, images from `receiver/`. The C3 has two firmware variants; read the esptool `USB mode` line: `USB-Serial/JTAG` means native USB (`sdkconfig.c3usb`), otherwise it is UART / WCH serial (`sdkconfig.c3uart`). Flash the wrong variant and the bridge receives no data.

The [Flash tool](../FLASH.md) applies these rules automatically; follow the criteria above when flashing manually.

The real boards on 2026-10-06 were: made `/dev/cu.usbmodem1101` (MAC `84:c7:bb:78:e4:30`), receiver `/dev/cu.usbmodem1301` (MAC `10:20:ba:ce:f4:1c`, native USB, not WCH `1a86:55d3`).

The local ESP-IDF 5.5 bundles esptool 4.12, which uses underscored arguments: `--before default_reset --after hard_reset write_flash`. Run `source "$HOME/esp/esp-idf/export.sh"` first, then use its `python -m esptool`. The `default-reset` / `write-flash` spellings in `receiver/release/FLASH.md` and `receiver/release-esp32c3/FLASH.md` are for newer esptool and fail outright on 4.12. Never erase the whole flash — it wipes Wi-Fi, pairing and the receiver hotspot password from NVS.

## Flashing made

The CMake caches in `made/build` and `made/build-c3usb` still point at the pre-rename `firmware-brookesia`. `build-c3usb` is only a historical directory name; its target chip is esp32s3. Never incrementally build in either. `made/release/` has no flashable image. Create a fresh build directory under `made/`:

```sh
source "$HOME/esp/esp-idf/export.sh"
cd made
idf.py -B build-s3 build
idf.py -B build-s3 -p /dev/cu.usbmodemXXXX flash
```

`idf.py flash` writes, per that directory's `flasher_args.json`: bootloader (`0x0`), partition table (`0x8000`), factory app `01_factory.bin` (`0x20000`), `ota_data_initial.bin` (`0xd000`), `assets.bin` (`0xac0000`) and `srmodels.bin` (`0xec0000`). If partitions are unchanged and only the app changed, writing just `0x20000` is enough; top up assets and srmodels when on-screen fonts or voice models go missing. Writing the app without assets leaves fonts missing on screen.

## On-device screenshot debugging

No need to photograph the screen with a phone: the firmware supports the `debug.screenshot` command (renders the current frame via `lv_snapshot` on the LVGL thread, POSTs the raw RGB565 frame to `/device/screenshot`), and the bridge converts it to PNG under `stateDir/screenshots/`, keeping the latest 20.

```sh
npm run shot                    # saves to state/last-shot.png, prints the path
npm run shot -- /tmp/shot.png   # custom output path
```

Prerequisites: the board is paired, the bridge is online, and the firmware capability list declares `debug.screenshot` (older firmware reports "command not in board list" — flash a newer build; the firmware also requires `CONFIG_LV_USE_SNAPSHOT=y`, already in sdkconfig.defaults). Implementation: `bridge/src/board-screenshot.mjs` (PNG encoding + storage) and `startScreenshotUpload()` in `esp_brookesia_app_vibe.cpp`. The desktop workbench can also fetch a PNG directly via `POST /api/screenshot` (with the session token).

## Flashing the receiver

The current C3 board uses native USB. The CMake cache in `receiver/build-c3usb` still says `receiver-firmware` — do not incrementally build it. `receiver/release/` is the ESP32-S3 receiver image: it must not be flashed to this C3, nor to the made host. Create a fresh build directory:

```sh
source "$HOME/esp/esp-idf/export.sh"
cd receiver
idf.py -B build-c3usb -D SDKCONFIG=sdkconfig.c3usb \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c3;sdkconfig.defaults.esp32c3usb" \
  -D IDF_TARGET=esp32c3 build
idf.py -B build-c3usb -D SDKCONFIG=sdkconfig.c3usb -p /dev/cu.usbmodemXXXX flash
```

When images already exist in that build directory, write them with esptool 4.12 and verify hashes:

```sh
python -m esptool --chip esp32c3 -p /dev/cu.usbmodemXXXX -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x10000 vibe_receiver.bin
```

After flashing, run `npm start` again. The workbench's "USB connection" should show the receiver connected, the hotspot name, and `http://192.168.4.1:8788`. With both boards plugged in, the bridge opens every `303a:1001`; made's own boot log appearing in the bridge output does not mean the receiver handshake failed. A healthy receiver keeps printing `hello: mode=receiver`. After made is unlocked (swipe up), pick the workbench's hotspot under gear → connection → receiver, then verify the six-digit code.
