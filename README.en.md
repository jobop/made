# made · 码得

[简体中文](README.md) · English

**made** is a voice companion for desktop coding agents. The current hardware is the Waveshare **ESP32-S3-Touch-LCD-1.85B** with a 360 × 360 touch display. The product and communication protocol do not depend on a round display.

Record a request, review the transcript, and confirm before a coding agent runs it on your computer. A **task is a session**: create it explicitly, then continue the conversation in the same task. Its first message becomes the title. Xiaozhi remains a separate application with its own voice connection.

## Boot and unlock

The desktop contains only **Xiaozhi, 码得, and Settings**. Boot opens the made lock screen automatically; opening or resuming made also starts locked. A green bean dances while the captions alternate between **码得，代码触手可得** and **Made,when you want to make**. **Swipe up to unlock** and enter the existing assistant, task, and settings screens.

While locked, made does not connect to the bridge or send heartbeats. Single, double, and triple BOOT presses do not record or act on tasks. Hold BOOT for about 1.2 seconds to return to the desktop, whether locked or unlocked. The new desktop, startup, and lock-screen behavior is pending physical verification.

## Start the computer bridge

Requires Node.js 22 or later. No npm dependencies are needed to run the bridge.

```sh
cp config.example.json config.local.json
npm start
```

Open [the dashboard](http://127.0.0.1:8787/). The three pages are **Dashboard**, **Assistants**, and **Tasks**. The English dashboard caption is **Made,when you want to make**; Tasks keeps **One task. An ongoing conversation.** Use the language selector in the header to switch between **简体中文** and **English**. The bridge saves your preference. The desktop launcher label remains **码得** (made) in both languages. On the device, Settings contains **Connection** and **Language**. Open **Language** to choose **中文 / English / Follow PC**. You can select a language before connecting; a manual selection is remembered and is not overwritten by the computer. Follow PC follows the bridge during configuration refresh and remembers the last language for offline use.

Set `projects` in `config.local.json` to the Git repositories you want to use. The included `demo-project` is available for trying the workflow. The bridge accepts existing uncommitted changes when starting a new task or continuing a session; review changes in your editor as needed. Existing project labels are user data, so the language switch preserves them; you can edit those labels in the configuration file.

Choose a model supported by each coding agent's account on the Assistants page. Install and sign in to the selected agent on your computer. The bridge lists installed **plugins**, with availability shown separately; it does not scan every application on your computer.

## Speech recognition

On the dashboard, configure the full **transcription URL**, **API key**, and **model**. The service must support an OpenAI-compatible audio transcription request:

- HTTP POST with Bearer authentication.
- `multipart/form-data` containing `file` (WAV audio) and `model`.
- A JSON response containing a `text` string.

OpenAI and SiliconFlow presets are provided. A service that only supports chat completion is not sufficient. Audio is recorded first and then uploaded; this is not live streaming transcription. Credentials stay on the computer.

The bridge does not force a spoken language. Recognition depends on the service and model. Interface language does not translate task titles, your instructions, or agent replies.

## Connect made

### USB direct — one board

Connect made to your computer with a data cable. Open **码得 → Settings → Connection → USB direct**, save, and keep the computer bridge running. No device Wi-Fi connection is needed. For first-time pairing, choose **Add made** on the dashboard, compare the six-digit codes, and approve on the computer.

### Local Wi-Fi

Connect the board through the desktop's Wi-Fi settings. In made, select Wi-Fi discovery and choose your computer from the discovered list. Compare the pairing codes before confirming. Discovery uses UDP `8789`; device communication uses TCP `8788`.

### Manual address / public connection

In made's connection settings, enter the bridge address and port. For a trusted LAN, use the computer's private IPv4 address and port `8788`. For a public address, select HTTPS, normally on port `443`; its certificate must match the domain or IP address.

The dashboard supports Cloudflare Tunnel, Tailscale Funnel, ngrok, and an existing Oray mapping. Each requires the relevant client/account setup. The tunnel publishes the device API only; the management page stays on `127.0.0.1:8787`. A public tunnel does not provide internet access to a board that cannot join Wi-Fi.

### Optional ESP32-S3 / ESP32-C3 receiver

A second ESP32-S3 with native USB, or the supported ESP32-C3 with WCH USB-to-UART, can provide a dedicated Wi-Fi hotspot and forward device traffic over USB. The dashboard displays its SSID and password. In made, open **Settings → Connection → Receiver**, scan and select the hotspot, then enter the eight-digit password shown on the PC. The receiver remembers its password across restarts. Manual entry remains available.

The receiver is optional, and the computer bridge is still required. Inserting a receiver does not automatically run programs on the computer. Physical testing with two boards is pending; see the [receiver documentation](receiver-firmware/README.md) before flashing.

## Device controls

Swipe up from the lock screen first. Once unlocked:

- Swipe left or right to switch assistants.
- Tap **+** to create a task; the first spoken request becomes its title.
- Use the task heading to switch tasks. Scroll the reply area to read more.
- Press **BOOT once** to record. Upload starts after you stop speaking.
- Press **BOOT twice** to confirm the displayed request.
- Press **BOOT three times** to cancel.
- Hold **BOOT** for about 1.2 seconds to return to the desktop.

The device connects and sends heartbeats only while made is in the foreground and unlocked. Locking, leaving the app, or rebooting stops bridge communication until the next unlock. Its pairing credentials remain saved when you leave. Xiaozhi's controls and language settings are independent.

## Extend and build

- [Plugin development](docs/PLUGINS.md): assistant metadata, capabilities, icons, sessions, progress, and cancellation.
- [Localization](docs/I18N.md): language ownership, translation boundaries, and adding strings.
- [Device firmware](firmware-brookesia/README.md) and [flashing instructions](firmware-brookesia/release/FLASH.md).

Assistant plugins run on the computer. Their names and icon pixels are sent to made, so adding a compatible plugin does not require another firmware update. Speech recognition uses the shared transcription interface rather than per-provider plugins.

The current firmware contains Chinese and English interface resources. Older firmware must be updated once to support language switching. Subsequent switching between the included languages does not require flashing.

```sh
npm test
```

Tests use temporary state and simulated agents/services. WorkBuddy has a shared external conversation interface and cannot guarantee task context isolation; its model must be selected in WorkBuddy. Read the detailed documentation for other implementation and hardware verification limits.

## Phone connection setup

On made, open **Settings → Connect → Phone setup**. Scan the Wi-Fi QR code with your phone camera and join the temporary `Made-Setup-xxxx` hotspot. If the setup page does not open, visit `http://192.168.8.1` on your phone. The screen also shows the temporary Wi-Fi password for manual joining.

Use the phone page to enter ordinary Wi-Fi credentials, receiver hotspot credentials, or paste a public HTTPS bridge URL. **Save and connect** closes the hotspot and applies the settings. Then check the connection and any six-digit pairing prompt on made. Public connections still need an internet-capable Wi-Fi network.

The device hosts this temporary page, so setup works before connecting to a computer. It closes on Back, app exit, or after 10 minutes. Speech API keys, models, and task management remain in the computer bridge.
