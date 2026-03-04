import requests
import pytest
import time
from helpers import step


def test_info_structure(base_url):
    """GET /info — structure and sanity"""
    resp = requests.get(f"{base_url}/info")
    step("returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    data = resp.json()
    step("body is a JSON object", isinstance(data, dict))
    for key in ("uptime", "heap_free", "deviceid"):
        step(f"has '{key}' field", key in data)
    step("uptime is non-negative", int(data["uptime"]) >= 0, str(data["uptime"]))


def test_info_update(base_url):
    """GET /info — uptime advances between two calls"""
    resp1 = requests.get(f"{base_url}/info").json()
    step("first call returns uptime", "uptime" in resp1)
    time.sleep(2)
    resp2 = requests.get(f"{base_url}/info").json()
    step("second call returns uptime", "uptime" in resp2)
    step(
        "uptime does not decrease",
        int(resp2["uptime"]) >= int(resp1["uptime"]),
        f"{resp1['uptime']} -> {resp2['uptime']}",
    )


def test_config_read(base_url):
    """GET /config — can be read and is non-empty JSON object"""
    resp = requests.get(f"{base_url}/config")
    step("returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    data = resp.json()
    step("body is a JSON object", isinstance(data, dict))
    step("object is non-empty", len(data) > 0, f"{len(data)} keys")


def test_data_read(base_url):
    """GET /data — can be read and is non-empty JSON object"""
    resp = requests.get(f"{base_url}/data")
    step("returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    data = resp.json()
    step("body is a JSON object", isinstance(data, dict))
    step("object is non-empty", len(data) > 0, f"{len(data)} keys")


def test_config_write(base_url):
    """POST /config — partial update of general.device_name is persisted"""
    # Read current value
    original = requests.get(f"{base_url}/config").json()
    original_name = original.get("general", {}).get("device_name", "")
    step("GET /config returns device_name", "general" in original)

    test_name = "pytest_write_test"
    resp = requests.post(f"{base_url}/config", json={"general": {"device_name": test_name}})
    step("POST /config returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")

    updated = requests.get(f"{base_url}/config").json()
    got_name = updated.get("general", {}).get("device_name", "")
    step("device_name was updated", got_name == test_name, f"expected {test_name!r}, got {got_name!r}")

    # Restore
    requests.post(f"{base_url}/config", json={"general": {"device_name": original_name}})
    restored = requests.get(f"{base_url}/config").json()
    restored_name = restored.get("general", {}).get("device_name", "")
    step("device_name restored to original", restored_name == original_name,
         f"expected {original_name!r}, got {restored_name!r}")


def test_data_write(base_url):
    """POST /data — partial update of sync-lock.id is persisted"""
    original = requests.get(f"{base_url}/data").json()
    step("GET /data returns sync-lock", "sync-lock" in original)
    original_id = original.get("sync-lock", {}).get("id", "")

    test_id = "pytest_sync_id"
    resp = requests.post(f"{base_url}/data", json={"sync-lock": {"id": test_id}})
    step("POST /data returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")

    updated = requests.get(f"{base_url}/data").json()
    got_id = updated.get("sync-lock", {}).get("id", "")
    step("sync-lock.id was updated", got_id == test_id, f"expected {test_id!r}, got {got_id!r}")

    # Restore
    requests.post(f"{base_url}/data", json={"sync-lock": {"id": original_id}})
    restored = requests.get(f"{base_url}/data").json()
    restored_id = restored.get("sync-lock", {}).get("id", "")
    step("sync-lock.id restored to original", restored_id == original_id,
         f"expected {original_id!r}, got {restored_id!r}")
