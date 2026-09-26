#!/usr/bin/env python3
"""Harness: drive mesh-mapper's update_detection + webhook state through the
mixed-feed scenario (node GPS frames interleaved with 1Hz coord-less bridge
frames for the same basic_id).

Checks:
  S1  first GPS detection for a new MAC -> exactly one alert
  S2  1Hz coord-less bridge posts (same basic_id) do NOT stomp GPS coords
  S3  node GPS re-report after coord-less posts does NOT re-alert (flap test)
  S4  coord-less post after a GPS fix still does not stomp
  S5  a genuinely new MAC still alerts exactly once

Run: python3 test_stale_flap.py /path/to/mesh-mapper.py
"""
import importlib.util
import sys
import time

path = sys.argv[1]
spec = importlib.util.spec_from_file_location("meshmapper", path)
mm = importlib.util.module_from_spec(spec)
sys.modules["meshmapper"] = mm
sys.argv = ["mesh-mapper.py", "--headless"]
spec.loader.exec_module(mm)

mm.webhook_url = None
mm.WEBHOOK_URL = None
triggers = []
orig_trigger = mm.trigger_backend_webhook_earliest
def spy(det, is_new):
    triggers.append({"mac": det.get("mac"), "is_new": is_new,
                     "drone_lat": det.get("drone_lat") or 0})
    return orig_trigger(det, is_new)
mm.trigger_backend_webhook_earliest = spy

failures = []

def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}")
    if not cond:
        failures.append(name)

def node_alerts():
    return [t for t in triggers if t["mac"] == NODE_MAC and t["drone_lat"]]

NODE_MAC = "aa:bb:cc:dd:ee:01"
BRIDGE_MAC = "02:00:00:00:00:01"
BID = "TEST-BID-0001-DRONE"

def node_frame(lat=35.5, lon=-78.9):
    return {"mac": NODE_MAC, "basic_id": BID, "rssi": -45,
            "drone_lat": lat, "drone_long": lon, "drone_altitude": 30}

def bridge_frame():
    return {"mac": BRIDGE_MAC, "basic_id": BID, "rssi": -70}

entry_lat = lambda: (mm.tracked_pairs.get(NODE_MAC) or {}).get("drone_lat")

# S1
mm.update_detection(node_frame())
check("S1 new GPS MAC alerts exactly once", len(node_alerts()) == 1,
      f"alerts={len(node_alerts())}")

# S2
for _ in range(10):
    mm.update_detection(bridge_frame())
check("S2 coord-less posts do not stomp GPS coords", entry_lat() == 35.5,
      f"drone_lat={entry_lat()}")
check("S2b no alert raised by coord-less posts alone", len(node_alerts()) == 1,
      f"alerts={len(node_alerts())}")

# S3
mm.update_detection(node_frame(lat=35.6))
check("S3 GPS re-report does not flap a new alert", len(node_alerts()) == 1,
      f"alerts={len(node_alerts())}")

# S4
mm.update_detection(bridge_frame())
check("S4 later coord-less post still preserves coords", entry_lat() == 35.6,
      f"drone_lat={entry_lat()}")

# S5
NEW_MAC = "cc:cc:cc:cc:cc:99"
mm.update_detection({"mac": NEW_MAC, "basic_id": "TEST-BID-9999",
                     "drone_lat": 36.1, "drone_long": -79.0})
new_alerts = [t for t in triggers if t["mac"] == NEW_MAC]
check("S5 genuinely new MAC alerts exactly once", len(new_alerts) == 1,
      f"alerts={len(new_alerts)}")

check("S3b active-state not corrupted by coord-less frames",
      mm.backend_previous_active.get(NODE_MAC) is True,
      f"previous_active={mm.backend_previous_active.get(NODE_MAC)}")

print()
print("RESULT:", "ALL PASS" if not failures else f"{len(failures)} FAILURES: {failures}")
sys.exit(1 if failures else 0)
