# Connection Modes

Made supports four connection methods, all using the same device API (event reporting ⇄ command queue). Switching methods requires no re-developing beyond provisioning:

| Method | Best For | Requires Wi-Fi? |
| --- | --- | --- |
| **USB Cable (Direct)** | No LAN, debugging | No |
| **LAN Wi-Fi (Direct)** | Device and computer on same network | Yes |
| **Receiver Relay** | Company Wi-Fi with portal/auth | Receiver hotspot only |
| **Public HTTPS (Tunnel)** | Remote access across networks | Yes (needs internet) |

**Pairing process is identical for all four:** Computer dashboard taps "Add Made" → Board picks target → **Verify 6-digit numbers on both ends** → Computer confirms.

## Pairing & Credentials

- Computer saves pairing tokens to `state/paired-devices.json`; board saves token, computer ID, and address to NVS.
- **No need to fill computer IP or device token at compile time; no `VIBE_DEVICE_TOKEN` needed.**
- Pairing interface: `POST /pair/request`, `GET /pair/status`; manual address paths verify saved token against target bridge with `GET /pair/verify` before each connection.
- First-device discovery auto-opens a ~2-minute pairing window; window expires, tap "Add Made" on the computer to reopen. Computer firewall needs UDP `8789` (discovery) and TCP `8788` (device API) open.
- After pairing, the board re-verifies bridge identity each time entering "Made" and unlocking before reusing the token; on failure, falls back to 6-digit code pairing. Switch computers: in the connection settings, choose Wi-Fi to rescan; scanning, refreshing, and exiting don't auto-switch to another computer.
- Only the "Made" app in foreground and unlocked, the board pings `GET /device/heartbeat` every ~12s (`HEARTBEAT_INTERVAL_MS = 12000`). Lock screen, app exit, or reboot while unlocked stops the heartbeat and command fetching; dashboard shows "offline." USB and receiver links timeout after ~20s without handshake.

---

## LAN Wi-Fi (Direct)

1. Connect computer and Made to the same Wi-Fi; start the bridge (`npm start` from repo root).
2. On the board's "Made" → Gear → Connection settings, choose **Wi-Fi Auto Discovery**, scan, then tap the target computer.
3. Verify the 6-digit numbers on both ends; confirm on the computer dashboard.

---

## Manual Address & Public Access

In connection settings, choose **Manual Address**, fill the bridge's IPv4/domain, port, and protocol:

- **Trusted LAN:** Fill computer's private IPv4, port `8788`, HTTP. Plain HTTP may be eavesdropped on the same network; use only on trusted networks.
- **Public:** Must choose **HTTPS** (usually `443`); certificate must match the filled domain or IP (when filling a public IP, cert's SAN must include that IP). Prefer a domain. **Don't map a plain HTTP port directly to the public.**

### No Public IP? (Tunneling)

The computer dashboard's "Public Access" supports publishing the **device interface `127.0.0.1:8788`** as a public HTTPS address; the management page always listens locally at `127.0.0.1:8787`. Tunneling doesn't auto-start with the bridge; toggle it manually on the dashboard.

- **Cloudflare (Temporary Trial):** Install [cloudflared](https://developers.cloudflare.com/tunnel/downloads/), try with [Quick Tunnel](https://developers.cloudflare.com/cloud-one/networks/connectors/cloudflare-tunnel/do-more-with-tunnels/trycloudflare/) — random address, may change.
- **Cloudflare (Fixed Domain):** Create a [Remote Management Tunnel](https://developers.cloudflare.com/tunnel/get-started/), target `http://127.0.0.1:8788`, `HTTP Host Header` override unset (changing to `localhost` causes address binding validation to fail). On the dashboard, fill the address and [Tunnel token](https://developers.cloudflare.com/tunnel/reference/tunnel-tokens/); token saves to computer's `state/tunnel-token` (0600 permissions).
- **Tailscale Funnel:** Install and login to Tailscale, enable MagicDNS on tailnet, configure HTTPS cert, and allow [Funnel](https://tailscale.com/docs/features/tailscale-funnel). The bridge publishes `8788` as HTTPS on `443` to a `*.ts.net` address; Made doesn't need Tailscale installed.
- **ngrok:** Install the [Agent](https://ngrok.com/docs/gateway/agent/getting-started/), fill the Authtoken on the dashboard; leave address blank for auto-assignment. The bridge verifies via [Agent API](https://ngrok.com/docs/gateway/agent/api) that the forward target is `127.0.0.1:8788` before displaying. Made requests include [ngrok's abuse-skip header](https://ngrok.com/abuse).
- **Peanut Shell (Oray):** Create an external `443 → 127.0.0.1:8788` HTTPS mapping on the Peanut Shell platform and grab the [API Key](https://service.oray.com/question/50663.html); the bridge just starts/stops this pre-existing, target-mapped tunnel, not adding/remapping. After enabling, use random-challenge verification that the public domain truly reaches the local machine.

**Common caveats:**

- **The board must have usable network and reach the public address;** company networks blocking the board's internet access mean tunnels won't substitute — switch to a Wi-Fi that can browse (e.g., phone hotspot).
- Public HTTPS validates cert chain, target host, and validity; the board pre-syncs network time in foreground mode; without trusted time, requests wait 15s before failing (tap BOOT to retry).
- Pairing still needs computer confirmation; don't share tunnel keys or device tokens.
- Standalone [DDNS](https://www.noip.com/support/knowledgebase/i-have-added-a-hostname-what-do-i-do-with_it-now) only updates domain to an existing public IP; under CGNAT it can't replace tunneling.

ngrok and Peanut Shell currently tested via mock interfaces; real account connectivity awaits user validation.

---

## USB Cable (Direct — One Made Only)

1. Plug Made into the computer via USB cable; on the board, **Connection settings → USB → Save**. Computer still runs the bridge; dashboard's "USB Connection" shows "Made USB Direct."
2. First time, pair via the regular 6-digit flow; already paired, verify computer identity and reuse the original pairing.

**Features & Boundaries:**

- No Wi-Fi, receiver, or tunnel needed; voice recognition and agents use the computer's own network.
- Uses native USB Serial/JTAG frame forwarding, **not keyboard simulation**, and doesn't auto-launch computer apps.
- USB identity saves separately: switching computers or restarting the bridge re-verifies; unverified, it doesn't send old tokens.
- Protocol connection builds only when app is in foreground, unlocked, and USB selected; exiting the app stops comms and cancels ongoing recordings; already-executed programming tasks remain under computer management. Pulling the cable shows "disconnected"; re-plug, verify, and resume.
- Firmware logs stay on UART0; USB secondary console is off, avoiding log contamination in the data protocol.

---

## Optional Receiver (Second Board)

When a company Wi-Fi requires web login or device onboarding, use a second ESP32-S3 (native USB) or ESP32-C3 as a receiver: it establishes its own WPA2 hotspot, forwarding Made's HTTP requests via USB serial to the computer.

1. Flash the receiver firmware (don't flash into the Made host): S3 see [receiver/release/FLASH.md](../receiver/release/FLASH.md); C3 native USB with `receiver-c3-usb`, WCH serial-to-USB with `receiver-c3` (see [receiver/release-esp32c3/FLASH.md](../receiver/release-esp32c3/FLASH.md)). **Flash the wrong variant and the bridge won't receive data;** on Windows, only native USB variants work ([PLATFORMS.md](PLATFORMS.md)).
2. Keep the bridge running; check dashboard's "USB Connection" for the hotspot name and 8-digit password (WPA2). macOS/Linux auto-discovery limits to Espressif `303a:1001` and WCH `1a86:55d3`; specify port with: `VIBE_USB_PORT=/dev/cu.usbmodemXXXX npm start`. If the Made host is also USB-connected, exit that mode first.
3. On the board, **Connection settings → Receiver**, scan and tap the hotspot, input the dashboard's password to save. After connecting, use the fixed address `http://192.168.4.1:8788`; no need to fill the computer IP.
4. Unpaired, tap "Add Made" on the computer and verify the 6-digit code. On the same computer, both transport methods reuse the verified pairing, syncing token updates and revocations.

**Boundaries:**

- The receiver is just a USB/Wi-Fi forwarder; **full functionality still needs the computer running the bridge;** the hotspot itself has no internet.
- Device connects to the receiver hotspot only when in foreground and unlocked; exit "Made" and it reverts to original Wi-Fi.
- C3 physical board flashed, validated, and USB-handshaked; hotspot connection, pairing, and full-voice chain await complete tuning.

---

## Mobile Provisioning (No Computer)

Made → Gear → Connection → **Mobile Config**. Phone scans the screen's QR code to join `Made-Setup-xxxx` temporary hotspot (usually auto-opens config page; otherwise open `http://192.168.8.1` in browser, or manually connect to the hotspot with its password).

The web page can fill a plain Wi-Fi, receiver hotspot info, or paste the bridge's full public HTTPS address; tap "Save & Connect" and Made closes the hotspot, applying settings, then follows the original 6-digit pairing flow.

- Provided by Made itself; no prior computer connection, no phone app needed.
- Entry valid only while config page is open; auto-closes after 10 minutes; returning or long-pressing BOOT exits also closes it.
- Voice API Key, model, and task management still configured on the computer workbench.
