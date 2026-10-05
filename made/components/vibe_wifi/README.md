# Vibe Wi-Fi service

This component starts ESP-IDF station mode after NVS initialization. Credential priority is:

1. Credentials explicitly saved in the `vibe_wifi` NVS namespace through `save_credentials()`.
2. An existing ESP-IDF station configuration in Wi-Fi NVS, which can preserve the current factory network if the factory firmware used ESP-IDF Wi-Fi storage and its NVS partition is retained.
3. Optional `CONFIG_VIBE_WIFI_SSID` and `CONFIG_VIBE_WIFI_PASSWORD` values entered locally through `idf.py menuconfig`.

The default build contains no SSID or password. If no credentials are found, the service starts Wi-Fi but does not attempt to connect. Wi-Fi disconnects trigger retry with delays from 1 to 30 seconds. The service never logs the password.

The optional USB receiver has separate `rx_ssid` and `rx_pass` keys in this namespace. Entering the Vibe app in receiver mode temporarily uses the receiver WPA2 SoftAP through the driver's RAM configuration. Leaving the app restores the previous station configuration. Receiver credentials do not replace the normal Wi-Fi credentials in NVS.

In `main/main.cpp`, include `vibe_wifi.hpp` and call `vibe_wifi::start()` after `nvs_flash_init()` and before app installation. Add `vibe_wifi` to main's `REQUIRES` list. A settings UI can call `vibe_wifi::save_credentials(ssid, password)` later.

Flashing without erasing NVS may retain the factory network, but this depends on how the factory firmware stored its credentials. Network connections need valid Wi-Fi credentials; USB direct mode does not. Receiver discovery and connection have been verified on the current display and C3 receiver.

## Temporary phone setup hotspot

`start_setup_ap()` creates a WPA2 `Made-Setup-xxxx` AP with a fresh eight-digit password and gateway `192.168.8.1`, retaining the station connection and saved credentials. Only one phone may connect. Automatic station retries and receiver scans pause during setup. `stop_setup_ap()` restores the previous Wi-Fi mode and retries; callers must check its result before switching station credentials. A separate `vibe_phone_setup` component provides HTTP and DNS. The application owns its foreground lifecycle and ten-minute timeout.
