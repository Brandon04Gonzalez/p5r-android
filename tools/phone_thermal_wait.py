#!/usr/bin/env python3
"""Waits until a phone is cool enough for a comparable performance run (docs/nvn/android-perf/PHASE4.md §2.2).

Reads `dumpsys thermalservice` (thermal status, HAL temperatures) and `dumpsys battery`, prints one line per check, and
exits 0 once the thermal status is 0, the battery is at most --battery °C and the skin at most --skin °C; 2 after
--timeout seconds without that. --once: one reading, exit 0.

usage: phone_thermal_wait.py <serial> [--battery 33.0] [--skin 34.0] [--timeout 900] [--interval 30] [--once]
"""
import argparse
import re
import subprocess
import sys
import time


def adb(serial, *args):
    r = subprocess.run(['adb', '-s', serial, 'shell', *args], capture_output=True, text=True, timeout=30)
    return r.stdout


def reading(serial):
    t = adb(serial, 'dumpsys', 'thermalservice')
    m = re.search(r'Thermal Status: (\d)', t)
    status = int(m.group(1)) if m else -1
    temps = {}
    section = t.split('Current temperatures from HAL:', 1)
    if len(section) == 2:
        for v, name in re.findall(r'mValue=([-\d.]+), mType=\d+, mName=([^,]+),', section[1].split('Current cooling', 1)[0]):
            temps[name] = float(v)
    b = adb(serial, 'dumpsys', 'battery')
    m = re.search(r'temperature: (\d+)', b)
    battery = int(m.group(1)) / 10 if m else temps.get('BAT', -1.0)
    m = re.search(r'level: (\d+)', b)
    level = int(m.group(1)) if m else -1
    skin = temps.get('SKIN', -1.0)
    ap = temps.get('AP', -1.0)
    return status, battery, skin, ap, level


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('serial')
    ap.add_argument('--battery', type=float, default=33.0)
    ap.add_argument('--skin', type=float, default=34.0)
    ap.add_argument('--timeout', type=float, default=900)
    ap.add_argument('--interval', type=float, default=30)
    ap.add_argument('--once', action='store_true')
    a = ap.parse_args()
    start = time.time()
    while True:
        status, battery, skin, soc, level = reading(a.serial)
        print(f'thermal: status {status}, battery {battery:.1f} °C, skin {skin:.1f} °C, AP {soc:.1f} °C, level {level}%',
              flush=True)
        if a.once:
            return 0
        if status == 0 and 0 <= battery <= a.battery and skin <= a.skin:
            return 0
        if time.time() - start > a.timeout:
            return 2
        time.sleep(a.interval)


if __name__ == '__main__':
    sys.exit(main())
