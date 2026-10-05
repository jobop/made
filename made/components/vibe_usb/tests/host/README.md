# USB transport host checks

Run the actual transport implementation against an in-memory USB peer, using
the project's cached cJSON source:

```sh
bash tests/host/run-tests.sh /path/to/managed_components/espressif__cjson/cJSON
```

The suite covers fragmented serial reads/writes, 1 KiB upload chunks, the maximum
256 KiB response, oversized frames and responses, length mismatch, stale request
IDs, per-connection and per-Bearer authorization, changed desktop IDs, cancellation
during upload and transcription, the pre-request cancellation race, timeouts,
disconnect and reconnection. The peer checks that direct hello contains no Wi-Fi
credentials. Exit must emit cancel and bye within one simulated 20 ms I/O slice.

The FreeRTOS task scheduler and USB driver are deterministic mocks; background
task scheduling, hardware timing and stack use still require ESP-IDF/board checks.
The real cJSON parser is linked; the mbedTLS base64 API is represented by a small
portable codec with known-vector checks.
