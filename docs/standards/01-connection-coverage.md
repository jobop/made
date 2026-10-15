# Connection Coverage Rule

**Every new feature, every interface, must work across ALL of the following connection paths:**

1. **USB cable direct** (no LAN; requests relayed over the USB serial frame protocol)
2. **LAN Wi-Fi direct** (device and computer on the same network; HTTP to port 8788)
3. **Receiver relay** (made joins the receiver's hotspot; traffic is relayed over USB serial to the computer)
4. **Public internet endpoint** (paired to a public HTTP/HTTPS endpoint; mind HTTPS certificates and the device time gate)

Concrete requirements:

- When adding a device-side API, consider all three transports at once: network HTTP (esp_http_client; for HTTPS attach `esp_crt_bundle` and check `tls_time_ready`), USB serial frames (`vibe_usb::request`, response cap 256 KB), and receiver/USB frame relaying (the forwarding verification chain in the bridge's `usb-receiver.mjs`).
- When adding a bridge endpoint, be explicit about which service it registers on (8788 device API / 8787 workbench) and confirm every connection path reaches the same port.
- If a feature cannot work over some connection path, the UI must say so explicitly — never fail silently.

For per-OS (macOS / Linux / Windows) availability, rationale and workarounds of each connection path, see [Platform support](../PLATFORMS.md). Run `npm run doctor` on the target machine for a direct local verdict (exit code 1 when a blocker exists).
