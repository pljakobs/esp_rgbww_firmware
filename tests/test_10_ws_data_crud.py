"""WebSocket /data CRUD tests — write via HTTP, read/verify via WS stream.

The WS ``data`` method is READ-ONLY (streaming export).  There is no WS write
API: the firmware's handleRequest() returns 501 for data-write requests, and
the wsMessageReceived() intercept for ``data`` always triggers an export stream.

Strategy for each test:
  1. Purge test fixture via HTTP (clean slate)
  2. Create / modify / delete via HTTP POST /data
  3. Fetch the full database snapshot via WS data stream
  4. Assert the expected state appears in the WS response

Helpers shared with test_08:
  _PRESET_ID / _GROUP_ID / _CTRL_ID / _SCENE_ID — same IDs, different
  test run so the device must be left clean.  _purge() calls retry until the
  item is gone so accumulated duplicates are handled.

WS IDs used: 60-79 (no conflicts with test_09 which uses 20-52).
"""
import json
import pytest
import requests
import requests.exceptions
from helpers import step, recv_stream

# ---------------------------------------------------------------------------
# Shared constants (match test_08 for cross-test traceability)
# ---------------------------------------------------------------------------

_PRESET_ID = "pytest_preset_1"
_GROUP_ID  = "pytest_group_1"
_CTRL_ID   = "pytest_ctrl_1"
_SCENE_ID  = "pytest_scene_1"
_TS        = 1700000000


# ---------------------------------------------------------------------------
# HTTP helpers (same as test_08)
# ---------------------------------------------------------------------------

class _TruncatedResponse:
    status_code = 200
    text = ""
    def json(self): return {"success": True}


def _data_post(base_url, payload: dict):
    try:
        return requests.post(f"{base_url}/data", json=payload)
    except requests.exceptions.ChunkedEncodingError:
        return _TruncatedResponse()


def _find_in(data: dict, key: str, item_id: str):
    for item in data.get(key, []):
        if item.get("id") == item_id:
            return item
    return None


def _purge(base_url: str, collection: str, item_id: str) -> None:
    """Remove ALL copies of item_id from collection (handles accumulated duplicates)."""
    for _ in range(20):
        data = requests.get(f"{base_url}/data").json()
        if _find_in(data, collection, item_id) is None:
            break
        _data_post(base_url, {f'{collection}[id={item_id}]': []})


# ---------------------------------------------------------------------------
# WS helper
# ---------------------------------------------------------------------------

def _ws_data_get(ws_client, req_id: int) -> dict:
    """Fetch the full database snapshot via the WS streaming data export."""
    ws_client.send(json.dumps({
        "jsonrpc": "2.0",
        "method": "data",
        "id": req_id,
    }))
    raw = recv_stream(ws_client, req_id)
    return json.loads(raw.decode("utf-8"))


# ---------------------------------------------------------------------------
# Preset CRUD via WS read
# ---------------------------------------------------------------------------

class TestWSPresetCRUD:
    def test_ws_create_preset(self, ws_client, base_url):
        """Create preset via HTTP, verify it appears in WS data stream"""
        _purge(base_url, "presets", _PRESET_ID)

        _data_post(base_url, {
            "presets[]": [{
                "id": _PRESET_ID, "name": "ws pytest warm white",
                "ts": _TS, "favorite": False, "icon": "palette",
                "color": {"hsv": {"h": 30, "s": 10, "v": 100, "ct": 2700}}
            }]
        })

        data = _ws_data_get(ws_client, 60)
        step("WS data stream received", data is not None)
        item = _find_in(data, "presets", _PRESET_ID)
        step("created preset appears in WS data", item is not None, "not found")
        if item:
            step("preset name matches via WS",
                 item.get("name") == "ws pytest warm white",
                 repr(item.get("name")))
            step("preset hsv.h matches via WS",
                 item.get("color", {}).get("hsv", {}).get("h") == 30,
                 str(item.get("color")))

    def test_ws_update_preset(self, ws_client, base_url):
        """Update preset name via HTTP, verify update appears in WS data stream"""
        _purge(base_url, "presets", _PRESET_ID)
        _data_post(base_url, {
            "presets[]": [{"id": _PRESET_ID, "name": "original", "ts": _TS,
                "favorite": False,
                "color": {"hsv": {"h": 60, "s": 100, "v": 80, "ct": 3000}}}]
        })

        _data_post(base_url, {
            f'presets[id={_PRESET_ID}]': {"name": "updated via ws test"}
        })

        data = _ws_data_get(ws_client, 61)
        item = _find_in(data, "presets", _PRESET_ID)
        step("updated preset name appears in WS data",
             item is not None and item.get("name") == "updated via ws test",
             repr(item.get("name") if item else None))

    def test_ws_delete_preset(self, ws_client, base_url):
        """Delete preset via HTTP, verify it is absent in WS data stream"""
        _purge(base_url, "presets", _PRESET_ID)
        _data_post(base_url, {
            "presets[]": [{"id": _PRESET_ID, "name": "to delete", "ts": _TS,
                "favorite": False,
                "color": {"hsv": {"h": 0, "s": 0, "v": 100, "ct": 6000}}}]
        })
        _purge(base_url, "presets", _PRESET_ID)

        data = _ws_data_get(ws_client, 62)
        item = _find_in(data, "presets", _PRESET_ID)
        step("deleted preset is absent in WS data", item is None,
             f"still found: {item}")


# ---------------------------------------------------------------------------
# Group CRUD via WS read
# ---------------------------------------------------------------------------

class TestWSGroupCRUD:
    def test_ws_create_group(self, ws_client, base_url):
        """Create group via HTTP, verify it appears in WS data stream"""
        _purge(base_url, "groups", _GROUP_ID)

        _data_post(base_url, {
            "groups[]": [{"id": _GROUP_ID, "name": "ws pytest group",
                "ts": _TS, "icon": "light_groups", "controller_ids": []}]
        })

        data = _ws_data_get(ws_client, 63)
        item = _find_in(data, "groups", _GROUP_ID)
        step("created group appears in WS data", item is not None, "not found")
        if item:
            step("group name matches via WS",
                 item.get("name") == "ws pytest group",
                 repr(item.get("name")))

    def test_ws_update_group(self, ws_client, base_url):
        """Update group name via HTTP, verify update appears in WS data stream"""
        _purge(base_url, "groups", _GROUP_ID)
        _data_post(base_url, {
            "groups[]": [{"id": _GROUP_ID, "name": "original group",
                "ts": _TS, "icon": "light_groups", "controller_ids": []}]
        })

        _data_post(base_url, {
            f'groups[id={_GROUP_ID}]': {"name": "renamed via ws test"}
        })

        data = _ws_data_get(ws_client, 64)
        item = _find_in(data, "groups", _GROUP_ID)
        step("updated group name appears in WS data",
             item is not None and item.get("name") == "renamed via ws test",
             repr(item.get("name") if item else None))

    def test_ws_delete_group(self, ws_client, base_url):
        """Delete group via HTTP, verify it is absent in WS data stream"""
        _purge(base_url, "groups", _GROUP_ID)
        _data_post(base_url, {
            "groups[]": [{"id": _GROUP_ID, "name": "to delete",
                "ts": _TS, "icon": "light_groups", "controller_ids": []}]
        })
        _purge(base_url, "groups", _GROUP_ID)

        data = _ws_data_get(ws_client, 65)
        item = _find_in(data, "groups", _GROUP_ID)
        step("deleted group is absent in WS data", item is None,
             f"still found: {item}")


# ---------------------------------------------------------------------------
# Controller CRUD via WS read
# ---------------------------------------------------------------------------

class TestWSControllerCRUD:
    def test_ws_create_controller(self, ws_client, base_url):
        """Create controller via HTTP, verify it appears in WS data stream"""
        _purge(base_url, "controllers", _CTRL_ID)

        _data_post(base_url, {
            "controllers[]": [{"id": _CTRL_ID, "name": "ws pytest ctrl",
                "ip-address": "192.168.99.99",
                "icon": "lights/led-strip-variant", "ts": _TS}]
        })

        data = _ws_data_get(ws_client, 66)
        item = _find_in(data, "controllers", _CTRL_ID)
        step("created controller appears in WS data", item is not None, "not found")
        if item:
            step("controller ip-address matches via WS",
                 item.get("ip-address") == "192.168.99.99",
                 repr(item.get("ip-address")))

    def test_ws_update_controller(self, ws_client, base_url):
        """Update controller name via HTTP, verify update appears in WS data stream"""
        _purge(base_url, "controllers", _CTRL_ID)
        _data_post(base_url, {
            "controllers[]": [{"id": _CTRL_ID, "name": "old name",
                "ip-address": "192.168.99.99",
                "icon": "lights/led-strip-variant", "ts": _TS}]
        })

        _data_post(base_url, {
            f'controllers[id={_CTRL_ID}]': {"name": "new name via ws test"}
        })

        data = _ws_data_get(ws_client, 67)
        item = _find_in(data, "controllers", _CTRL_ID)
        step("updated controller name appears in WS data",
             item is not None and item.get("name") == "new name via ws test",
             repr(item.get("name") if item else None))

    def test_ws_delete_controller(self, ws_client, base_url):
        """Delete controller via HTTP, verify it is absent in WS data stream"""
        _purge(base_url, "controllers", _CTRL_ID)
        _data_post(base_url, {
            "controllers[]": [{"id": _CTRL_ID, "name": "to delete",
                "ip-address": "192.168.99.99",
                "icon": "lights/led-strip-variant", "ts": _TS}]
        })
        _purge(base_url, "controllers", _CTRL_ID)

        data = _ws_data_get(ws_client, 68)
        item = _find_in(data, "controllers", _CTRL_ID)
        step("deleted controller is absent in WS data", item is None,
             f"still found: {item}")


# ---------------------------------------------------------------------------
# Scene CRUD via WS read
# ---------------------------------------------------------------------------

class TestWSSceneCRUD:
    def test_ws_create_scene(self, ws_client, base_url):
        """Create scene via HTTP, verify it appears in WS data stream"""
        _purge(base_url, "scenes", _SCENE_ID)

        _data_post(base_url, {
            "scenes[]": [{
                "id": _SCENE_ID, "name": "ws pytest evening",
                "ts": _TS, "group_id": _GROUP_ID,
                "favorite": False, "icon": "scene", "settings": []
            }]
        })

        data = _ws_data_get(ws_client, 69)
        item = _find_in(data, "scenes", _SCENE_ID)
        step("created scene appears in WS data", item is not None, "not found")
        if item:
            step("scene name matches via WS",
                 item.get("name") == "ws pytest evening",
                 repr(item.get("name")))

    def test_ws_update_scene(self, ws_client, base_url):
        """Update scene favorite field via HTTP, verify update appears in WS data stream"""
        _purge(base_url, "scenes", _SCENE_ID)
        _data_post(base_url, {
            "scenes[]": [{"id": _SCENE_ID, "name": "ws scene fav test",
                "ts": _TS, "group_id": _GROUP_ID, "favorite": False,
                "icon": "scene", "settings": []}]
        })

        _data_post(base_url, {
            f'scenes[id={_SCENE_ID}]': {"favorite": True}
        })

        data = _ws_data_get(ws_client, 70)
        item = _find_in(data, "scenes", _SCENE_ID)
        step("updated scene favorite appears in WS data",
             item is not None and item.get("favorite") is True,
             repr(item.get("favorite") if item else None))

    def test_ws_delete_scene(self, ws_client, base_url):
        """Delete scene via HTTP, verify it is absent in WS data stream"""
        _purge(base_url, "scenes", _SCENE_ID)
        _data_post(base_url, {
            "scenes[]": [{"id": _SCENE_ID, "name": "to delete",
                "ts": _TS, "group_id": _GROUP_ID, "favorite": False,
                "icon": "scene", "settings": []}]
        })
        _purge(base_url, "scenes", _SCENE_ID)

        data = _ws_data_get(ws_client, 71)
        item = _find_in(data, "scenes", _SCENE_ID)
        step("deleted scene is absent in WS data", item is None,
             f"still found: {item}")


# ---------------------------------------------------------------------------
# WS data read — structural sanity
# ---------------------------------------------------------------------------

def test_ws_data_stream_structure(ws_client, base_url):
    """WS data stream returns a valid JSON object with known top-level keys"""
    data = _ws_data_get(ws_client, 72)
    step("WS data stream parses as dict", isinstance(data, dict), str(type(data)))
    for key in ("presets", "groups", "controllers", "scenes"):
        step(f"WS data has '{key}' array",
             isinstance(data.get(key), list),
             f"{key!r}={data.get(key)!r}")


def test_ws_data_matches_http(ws_client, base_url):
    """WS data stream and GET /data return identical preset/group/scene/controller arrays"""
    http_data = requests.get(f"{base_url}/data").json()
    ws_data   = _ws_data_get(ws_client, 73)

    for key in ("presets", "groups", "controllers", "scenes"):
        http_ids = sorted(i.get("id", "") for i in http_data.get(key, []))
        ws_ids   = sorted(i.get("id", "") for i in ws_data.get(key, []))
        step(f"WS and HTTP '{key}' id lists match",
             http_ids == ws_ids,
             f"HTTP={http_ids} WS={ws_ids}")
