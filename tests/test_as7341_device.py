"""Opt-in integration tests against an AS7341-equipped controller."""

import asyncio
import json
import os
import urllib.request

import pytest


BASE = os.environ.get("AS7341_TEST_BASE_URL")


@pytest.mark.skipif(not BASE, reason="set AS7341_TEST_BASE_URL for an AS7341-equipped device")
def test_measurement_transports():
    websockets = pytest.importorskip("websockets")
    readings = f"{BASE}/extensions/spectrum-1/readings"
    websocket_url = BASE.replace("http://", "ws://", 1).replace("https://", "wss://", 1) + "/ws"

    async def run():
        async with websockets.connect(websocket_url) as first, websockets.connect(websocket_url) as second:
            await first.send(json.dumps({
                "jsonrpc": "2.0", "id": "invalid", "method": "extensions.start_measurement",
                "params": {"extension_id": "spectrum-1"},
            }))
            invalid = json.loads(await asyncio.wait_for(first.recv(), 5))
            assert invalid["error"]["code"] == -32602
            await first.send(json.dumps({
                "jsonrpc": "2.0", "id": 41, "method": "extensions.start_measurement",
                "params": {"extension_id": "spectrum-1"},
            }))
            accepted = json.loads(await asyncio.wait_for(first.recv(), 5))
            assert accepted["id"] == 41
            assert accepted["result"] == {"extension_id": "spectrum-1", "valid": False}
            result = json.loads(await asyncio.wait_for(first.recv(), 5))
            assert result["method"] == "extensions.measurement"
            assert result["params"]["valid"] is True
            assert result["params"]["state"] == "complete"
            assert set(result["params"]["data"]) == {
                "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8",
                "clear_low", "nir_low", "clear_high", "nir_high", "atime", "astep", "gain",
            }
            with pytest.raises(asyncio.TimeoutError):
                await asyncio.wait_for(second.recv(), 0.3)
            with urllib.request.urlopen(readings, timeout=5) as response:
                polled = json.load(response)
            assert polled["valid"] is True
            assert polled["data"] == result["params"]["data"]

    asyncio.run(run())
