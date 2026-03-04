import pytest
import toml
import requests
import json
from websocket import create_connection

def pytest_addoption(parser):
    parser.addoption("--config", action="store", default="tests/test_config.toml")

@pytest.fixture(scope="session")
def config(pytestconfig):
    config_path = pytestconfig.getoption("--config")
    try:
        return toml.load(config_path)
    except Exception as e:
        pytest.fail(f"Could not load config: {e}")

@pytest.fixture(scope="session")
def device_ip(config):
    return config['device']['ip']

@pytest.fixture(scope="session")
def base_url(device_ip):
    return f"http://{device_ip}"

@pytest.fixture(scope="session")
def ws_url(device_ip):
    return f"ws://{device_ip}/ws"

@pytest.fixture
def ws_client(ws_url):
    ws = create_connection(ws_url, timeout=5)
    yield ws
    ws.close()
