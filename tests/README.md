# API Tests - Version 2.0 (Pytest Structure)

## Structure
- `tests/conftest.py`: Fixtures for base_url, websocket client...
- `tests/test_01_basics.py`: HTTP GET /info, /config, /data
- `tests/test_02_consistency.py`: Cross-protocol verification (Does WS info == HTTP info?)
- `tests/test_03_http_control.py`: Set Color, WS Notifications, On/Off behavior.

## Execution
Run all tests:
```bash
pytest tests/
```

Run specific test:
```bash
pytest tests/test_02_consistency.py
```

## Status
- [ ] HTTP Reads (Basics)
- [ ] Consistency Checks (Config, Info, Color cross-check)
- [ ] Control Logic (Color, On/Off)
