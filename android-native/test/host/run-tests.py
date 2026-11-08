#!/usr/bin/env python3
"""Compile Android sources and run HTTP/HMAC boundary tests without an emulator."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--java-home', default=os.environ.get('JAVA_HOME'))
parser.add_argument('--android-sdk', default=os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT'))
parser.add_argument('--gson-jar', help='Override the Android command-line tools Gson jar used by host-only JSON shims')
args = parser.parse_args()
if not args.java_home:
    parser.error('Set JAVA_HOME or --java-home (JDK 17+)')
if not args.android_sdk:
    props = root / 'local.properties'
    if props.exists():
        args.android_sdk = next((line.split('=', 1)[1] for line in props.read_text().splitlines() if line.startswith('sdk.dir=')), None)
if not args.android_sdk:
    parser.error('Set ANDROID_HOME or --android-sdk')
sdk = Path(args.android_sdk)
gson = Path(args.gson_jar) if args.gson_jar else next(iter(sorted((sdk / 'cmdline-tools').rglob('gson-*.jar'))), None)
android_jar = sdk / 'platforms/android-34/android.jar'
if gson is None or not gson.is_file() or not android_jar.is_file():
    parser.error('Need Android 34 platform and command-line tools Gson (or --gson-jar)')
java_bin = Path(args.java_home) / 'bin'
print(f'JDK: {args.java_home}\nAndroid SDK: {sdk}\nHost JSON jar: {gson}', flush=True)
production = root / 'app/src/main/java'
host = root / 'test/host'
with tempfile.TemporaryDirectory(prefix='made-android-tests-') as directory:
    classes = Path(directory) / 'app'
    classes.mkdir()
    subprocess.run([str(java_bin / 'javac'), '-encoding', 'UTF-8', '-cp', str(android_jar), '-d', str(classes),
                    *map(str, production.rglob('*.java'))], check=True)
    print('PASS: compiled all Android app Java sources against Android 34', flush=True)
    classes = Path(directory) / 'host'
    classes.mkdir()
    files = list(host.rglob('*.java')) + [production / 'com/made/app' / name for name in ('Bridge.java', 'Pairing.java', 'EndpointScope.java')]
    subprocess.run([str(java_bin / 'javac'), '-encoding', 'UTF-8', '-cp', str(gson), '-d', str(classes), *map(str, files)], check=True)
    subprocess.run([str(java_bin / 'java'), '-cp', str(classes) + os.pathsep + str(gson), 'com.made.app.BoundaryTest'], check=True)
