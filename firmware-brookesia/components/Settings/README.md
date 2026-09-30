# Settings for ESP32-S3-Touch-LCD-1.85B

This page is adapted from the [Waveshare ESP32-S3-Touch-AMOLED-1.75 Brookesia Settings application](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/tree/main/firmware/brookesia/components/Settings), licensed Apache-2.0. The launcher icon and `ui/SettingsUI.hpp` derive from that project. The application logic was replaced for the 1.85B's 360×360 ST77916 screen, BQ27220 gauge, and shared `vibe_wifi` connection service.

Implemented pages:

- WLAN: scans nearby networks, lists up to eight SSIDs, accepts a password through the touch keyboard, and persists credentials through `vibe_wifi::save_credentials()`. Connection retries remain owned by `vibe_wifi` so Weather and Vibe Coding share the same network state.
- Display: adjusts the 1.85B PWM backlight for the current session.
- Power: shows read-only BQ27220 state of charge, voltage, and current, plus Wi-Fi connection state.
- Sound and About: basic information pages.

The original 1.75 Wi-Fi switch, storage diagnostics, persistent brightness setting, and audio volume control have not been ported. The latter two need integration with the board's shared settings and audio session layer; direct use of the 1.75 AXP2101 or TCA9554 code is unsafe on this board. The BQ27220 is read by register only: this page does not invoke the driver constructor that can update a battery calibration profile.

This code is a build candidate. Wi-Fi keyboard reachability and round-screen margins still need testing on the physical board.
