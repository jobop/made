# Android host boundary tests

Run with JDK 17+ and the Android 34 SDK:

```sh
python3 android-native/test/host/run-tests.py --java-home "$JAVA_HOME" --android-sdk "$ANDROID_HOME"
```

The runner prints the resolved JDK, SDK and host JSON jar. The SDK can also come from `android-native/local.properties`; override the host-only Gson dependency with `--gson-jar` if needed.

The suite first compiles every app Java source against the real Android 34 framework jar. It then runs production `Bridge`, `Pairing` and `EndpointScope` against a local HTTP server, with small Android logging/preferences shims and a host JSON adapter backed by the SDK toolchain's Gson. No test dependency is shipped in the app.

Checks cover credential origin/path boundaries, renewal, redirects, stale responses after logout, LAN and cloud-proxy HMAC verification, revocation, malformed signatures, and persistent cache namespace separation across endpoints/accounts/pairings. This does not replace emulator or physical-device WebView/UI validation.
