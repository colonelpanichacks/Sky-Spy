#!/usr/bin/env python3
"""Harness: drive tools/ds110_bridge.py's BLE callback with synthesized
ASTM F3411 advertisements and check the optional trust verdicts.

Checks:
  T1  BasicID + Location + System ads produce a tracked drone with a
      trust verdict attached (remoteid installed)
  T2  the POST payload carries the trust dict with verdict/alert/reasons
  T3  with remoteid unavailable the bridge behaves exactly as before:
      no trust field, no crash
  T4  a malformed relay (truncated service data) is ignored, never fatal

Run: python3 test_bridge_trust.py
(bleak is stubbed: the test needs no Bluetooth hardware and no bleak install)
"""
import os
import struct
import sys
import types

# --- stub bleak before importing the bridge (no hardware / no install) ------
bleak = types.ModuleType("bleak")
bleak.BleakScanner = object
backends = types.ModuleType("bleak.backends")
device_mod = types.ModuleType("bleak.backends.device")
scanner_mod = types.ModuleType("bleak.backends.scanner")


class BLEDevice:
    def __init__(self, address):
        self.address = address


class AdvertisementData:
    def __init__(self, service_data=None, rssi=-65):
        self.service_data = service_data or {}
        self.rssi = rssi


device_mod.BLEDevice = BLEDevice
scanner_mod.AdvertisementData = AdvertisementData
sys.modules.update({
    "bleak": bleak,
    "bleak.backends": backends,
    "bleak.backends.device": device_mod,
    "bleak.backends.scanner": scanner_mod,
})

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import ds110_bridge as bridge

failures = []


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}")
    if not cond:
        failures.append(name)


# --- synthesized ASTM F3411 messages (25 bytes, header byte first) ----------
def basic_id(uasid=b"1ABC123DEF456GHIJK78", id_type=1, ua_type=2):
    m = bytearray(25)
    m[0] = (0x0 << 4) | 2          # Basic ID, protocol version 2
    m[1] = ((id_type & 0xF) << 4) | (ua_type & 0xF)
    m[2:22] = uasid.ljust(20, b"\x00")[:20]
    return bytes(m)


def location(lat=37.7749, lon=-122.4194, alt_geo=120.0):
    m = bytearray(25)
    m[0] = (0x1 << 4) | 2          # Location/Vector
    m[1] = 0x20                    # status: airborne
    m[2] = 180                     # direction 90 deg
    m[3] = 40                      # speed 10 m/s
    struct.pack_into("<i", m, 5, int(lat * 1e7))
    struct.pack_into("<i", m, 9, int(lon * 1e7))
    struct.pack_into("<H", m, 13, int(100.0 / 0.5 + 1000))
    struct.pack_into("<H", m, 15, int(alt_geo / 0.5 + 1000))
    struct.pack_into("<H", m, 17, int(50.0 / 0.5 + 1000))
    return bytes(m)


def system_msg(plat=37.7750, plon=-122.4195):
    m = bytearray(25)
    m[0] = (0x4 << 4) | 2          # System
    m[1] = 0x01
    struct.pack_into("<i", m, 2, int(plat * 1e7))
    struct.pack_into("<i", m, 6, int(plon * 1e7))
    return bytes(m)


def adv_for(payload):
    # ds110 service data: application code + message counter + one message
    return AdvertisementData({bridge.ODID_UUID: b"\x0d\x00" + payload}, rssi=-65)


ADDR = "DC:A6:32:11:22:33"
have_remoteid = bridge.remoteid is not None
print(f"remoteid available: {have_remoteid}")

b = bridge.Bridge(url=None, list_only=True)
dev = BLEDevice(ADDR)
for payload in (basic_id(), location(), system_msg()):
    b.on_advertisement(dev, adv_for(payload))

drones = list(b.drones.values())
check("T1a one drone tracked", len(drones) == 1, f"(got {len(drones)})")
drone = drones[0]
check("T1b basic_id decoded", drone.basic_id == "1ABC123DEF456GHIJK78",
      f"(got {drone.basic_id!r})")
if have_remoteid:
    check("T1c trust verdict attached", isinstance(drone.trust, dict),
          f"(got {drone.trust!r})")
    trust = drone.trust or {}
    check("T2a verdict value valid",
          trust.get("verdict") in ("authenticated", "plausible",
                                   "suspicious", "spoofed"),
          f"(got {trust.get('verdict')!r})")
    check("T2b is_alert is bool", isinstance(trust.get("is_alert"), bool))
    check("T2c reasons is list", isinstance(trust.get("reasons"), list))
    payload = drone.to_payload()
    check("T2d payload carries trust", payload.get("trust") == drone.trust)
else:
    print("[SKIP] T1c/T2 remoteid not installed")

# --- T3: graceful degradation with remoteid unavailable ---------------------
saved = bridge.remoteid
bridge.remoteid = None
try:
    b2 = bridge.Bridge(url=None, list_only=True)
    for payload in (basic_id(), location(), system_msg()):
        b2.on_advertisement(dev, adv_for(payload))
    drone2 = list(b2.drones.values())[0]
    check("T3a still tracks without remoteid", drone2.basic_id == "1ABC123DEF456GHIJK78")
    check("T3b no trust without remoteid", drone2.trust is None)
    check("T3c payload has no trust key", "trust" not in drone2.to_payload())
finally:
    bridge.remoteid = saved

# --- T4: malformed relays never kill the callback ---------------------------
b3 = bridge.Bridge(url=None, list_only=True)
try:
    b3.on_advertisement(dev, AdvertisementData({bridge.ODID_UUID: b"\x0d\x00\x10"}))
    b3.on_advertisement(dev, AdvertisementData({bridge.ODID_UUID: b"\x99" + b"\x00" * 26}))
    b3.on_advertisement(dev, AdvertisementData({}))
    check("T4 malformed ads ignored", True)
except Exception as e:
    check("T4 malformed ads ignored", False, f"({type(e).__name__}: {e})")

print()
if failures:
    print(f"{len(failures)} FAILURES: {failures}")
    sys.exit(1)
print("all checks passed")
