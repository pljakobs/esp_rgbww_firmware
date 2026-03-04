import toml
import requests
import json
import sys

try:
    config = toml.load("tests/test_config.toml")
    ip = config['device']['ip']
    base_url = f"http://{ip}"
except:
    base_url = "http://192.168.29.31"

print("--- CONFIG ---")
try:
    r = requests.get(f"{base_url}/config")
    print(json.dumps(r.json(), indent=2))
except Exception as e:
    print(e)

print("\n--- DATA ---")
try:
    r = requests.get(f"{base_url}/data")
    print(json.dumps(r.json(), indent=2))
except Exception as e:
    print(e)
