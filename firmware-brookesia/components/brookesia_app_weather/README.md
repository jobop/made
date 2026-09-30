# Brookesia Weather app

This is a separate ESP-Brookesia app for the 360 × 360 Waveshare ESP32-S3-Touch-LCD-1.85B. It does not replace XiaoZhi or other launcher apps.

- Tap the city name to enter an English city name (for example `Beijing, China`), then tap **Save**. The city is stored in NVS.
- The worker geocodes the city and reads Open-Meteo's `current.temperature_2m` and `current.weather_code` over HTTPS. The location and last successful weather are cached in NVS for offline display.
- Each HTTP request has a 6-second timeout; JSON responses are capped at 4095 bytes. The worker refreshes every 30 minutes, retries weather failures after 2 minutes, and polls Wi-Fi while disconnected.
- LVGL objects are accessed only on the GUI task. The network worker copies data into a mutex-protected snapshot.
- The app uses the shared `vibe_wifi` connection service; Wi-Fi credentials can be entered in the Settings app. Weather keeps its own last successful reading for offline display.

To register in `main/main.cpp`, include `esp_brookesia_app_weather.hpp` and call `phone->installApp(esp_brookesia::apps::Weather::requestInstance())` after `phone->begin()`. Add `brookesia_app_weather` to `main/CMakeLists.txt` `REQUIRES`. `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` and Montserrat fonts 12, 14, 16, 26 should be enabled.

API references: [Open-Meteo Forecast API](https://open-meteo.com/en/docs) and [Geocoding API](https://open-meteo.com/en/docs/geocoding-api). Open-Meteo's license and use limits apply to deployments.

The component has not yet been tested on the physical board.
