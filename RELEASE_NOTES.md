# Release Notes

## 2026-04-05

This release covers OTA hardening, crash reporting, heap tracking, extensive Lightinator Log Service development, and webapp compatibility and CI fixes. Work spans commits from 2026-04-04 and 2026-04-05 across all three repositories.

### Firmware

#### OTA hardening

- Fixed stuck OTA state: `saveStatus` now updates the member variable; `checkAtBoot` resets a stuck `OTA_PROCESSING` status on next boot (`6af5d10`).
- OTA now always reboots after any failure — no more stuck states (`4efb2c3`).
- Fixed failure status broadcast when target partition is already the running partition; reduced OTA watchdog to 2 minutes with automatic reboot on timeout (`94db91e`).
- Added `clear_ota_restart` API command (via `POST /system`) to programmatically unstick a stuck OTA status (`f7350c2`, `ec5401d`).

#### Crash reporting

- Capture stack trace via RTC memory and transmit via syslog after network becomes available (`be95ba0`).
- Fixed output format for compatibility with `decode-stacktrace` tooling (`34ccad0`).
- Fixed build errors; increased `CRASH_STACK_WORDS` to 54 (256 bytes); made `readCrashDump` public (`9545b8c`).

#### Heap tracking

- Added global free heap tracking with per-uptime and rolling 10-minute minimum trackers (`da26ee8`, `149a05d`).
- Added countable out-of-heap-space error counters exposed in `/info?v=2` (`2fe8607`).
- Simplified minimum heap checking in webserver; freed unreachable code paths (`28f2a4d`, `89a853f`).

#### Telemetry fixes

- Fixed `telemetryClient::stop` — MQTT session is now fully torn down on stop, preventing a crash when `start` is called again on an existing connection (`0225918`).
- Fixed telemetry being constantly re-enabled even when not configured (`0fbd775`).
- Aligned telemetry with network schema; prevented repeated rsyslog reapply on config writes (`897188f`, `af5076d`).

#### Schema

- Added `required: [id, name]` constraint to `scenes` items in `app-data.cfgdb` (`4222cda`).

#### CI

- ELF and map files are now bundled into firmware build artifacts alongside binaries, enabling post-hoc stack-trace analysis (`ad6ab9a`).
- Fixed CI matrix generation to use the `generate-matrix` job; single source of truth for supported SoCs (`c2f927f`, `952a674`).
- Staged artifacts flat before upload to fix 404s on binary downloads (`2d05f16`).
- CI dispatches `firmware-schema-update` event to the webapp repository on each tagged build (`7959a31`).
- Upgraded all GitHub Actions to Node 24-compatible versions (`51899d5`).

---

### Lightinator Log Service

Extensive development bringing the service from initial scaffolding to a full-featured logging dashboard with Loki integration and crash-decoding capability.

#### Core logging features

- Browser UI with Loki log forwarding support (`6b53b6f`).
- Controller discovery and logging dashboard — auto-discovers devices and displays live logs per controller (`afadfe3`).
- Configurable Loki tagging: per-group and per-controller label sets (`a782c06`).
- Reboot markers in log view; infinite scroll upwards to load older log entries (`2465853`).
- Controller list persists across service restarts; initial logging state is read from firmware `/config` (`4d689b2`).
- Added `host` label to Loki streams derived from the syslog tag (controller name) (`20dc392`).

#### Firmware integration

- Logging toggle in the UI pushes rsyslog configuration directly to the firmware via `POST /config` (`31f0775`).
- Auto-detect syslog advertise host at startup; `LLS_SYSLOG_ADVERTISE_HOST` environment variable override (`247129e`).
- `/hosts?all=true` endpoint returns complete controller inventory (`770f49b`).
- Version reported in health/status responses (`55ad344`).

#### Crash stack decode

- Integrated ELF toolchain and crash detector; crash decode tab greys out in non-debug builds where ELF is unavailable (`f6a1b8c`).
- Docker: pinned Sming build stage to `linux/amd64` for reproducible multi-platform images (`7fc94b2`).
- Docker: robust `addr2line` binary discovery with fallback; hard failure if tools are missing — no silent stubs (`e65c36f`, `40648a3`).

#### Configuration & deployment

- Central `service.env` config file; quadlet unit uses `EnvironmentFile` instead of inline environment variables (`073ebb6`).
- Service settings UI and API for runtime configuration (`1760060`).
- Interactive installer script with scope (user/system) selection (`9f99e54`).
- Prod quadlet with auto-update support; split-brain detection (`770f49b`).
- CI: publish `:prod` image on merge to the `prod` branch (production gate via PR) (`95aa75b`).
- Git-based version reporting; unified Settings panel with tabbed layout (`0ad8117`).
- install.sh fixes: use `start` not `enable --now` for quadlet units; fix variable substitution in `ask_scope`; remove unsupported `-` prefix from `EnvironmentFile`; use `/var/lib` path for system-scope install (`0f1ed39`, `efb740f`, `7528207`, `1677176`).

#### UI/UX

- Loki status indicator dot in header; reboot detection disabled until log timestamps are available (`db046f0`).
- Loki config persistence; controller link in dashboard; auto-switch to logs tab on controller select (`051c100`).
- Settings overlay DOM order fix; group ID type normalisation; sources panel merges discovery results; log-all buttons (`9729a1d`).
- Verbose Loki test output — show target URL and full response body on error (`2f0e1cb`).
- Pre-load controller list on page init, not only on tab click (`b0ca370`).

---

### Webapp (esp_rgb_webapp2)

#### API compatibility

- Implemented chunked `PATCH` updates for `/data` and `/config` to reduce heap pressure on constrained devices, especially ESP8266 (`2dccde7`, `849b3ae`).
- Firmware/frontend protocol compatibility fixes for the new telemetry schema (`dfc40cc`).
- Normalised telemetry config payloads; restored sync consolidation logic (`9f38bc0`, `a65978a`).
- Updated telemetry field documentation with heap pressure metrics (`d558025`).

#### Sync & schema integrity

- Schema-locked API writes with sync referential integrity enforcement (`58a7669`).
- Sync consistency check is now convergence-aware (retries until stable) (`7f8eec8`).
- CI: bidirectional firmware schema sync + schema lock + deploy race fix (`6183471`).
- CI: re-sync schema lock to crash-reporting SHA on each CI run; show diff output on integrity check failure (`3f7176e`, `d5fb796`).
- Added `schemaValidator.spec.js` to test workflow; validator now covers 171 schema items (`08f8b35`).

#### UI fixes

- Fixed wide-screen two-column layout in system settings (`8ffe724`).
- Fixed display error in `myCard` component on small screens (`c8f0ed2`).

---

### Firmware documentation

- Rewrote `README.md` comprehensively — full HTTP API reference (all endpoints), color command reference (HSV/RAW/relative values/multi-cmd/from-to/channels/queue policies), MQTT and Home Assistant discovery, multi-controller synchronisation with clock PLL details, rsyslog and Lightinator Log Service setup, WebSocket API, security, complete configuration reference, build instructions, and hardware pin configuration table.

## 2026-04-03

This release bundles firmware, webapp, and local tooling work delivered the same day for the new remote log pipeline and related UI/CI updates.

### Firmware

- Added UDP rsyslog logging capability in firmware (`eb5d81e`).
- Added hardware fade functionality updates in `RGBWWLed` and `ESP32HardwarePwm` (`78eea18`).
- Improved runtime behavior so rsyslog is reconfigured immediately when relevant config values change, without requiring reboot (`e6f86e3`, `5eefd46`).
- Included integration merges for the rsyslog feature branch (`a556261`, `8ef5f0a`, `90fbdf9`, `8479b60`, `27654d5`).

### Webapp

- Added rsyslog settings page support and related config UX improvements (`4d50674`, `496e5a0`, `9a572ce`).
- Modernized CI/build compatibility for Node.js and GitHub Actions (`f9d1c64`, `d81fb77`, `702f9a9`, `86066f4`, `8817bbe`).
- Completed wide viewport system settings layout work with true two-column behavior (`8c304de`, `1ac503e`).
- Updated firmware selection dialog to the nested info store format (`5952f1d`).
- Added compatibility fallback for controller info endpoint usage (`6a9ce67`).
- Integrated local log collector in Log Viewer with live tail, paging, fullscreen, and setup guidance (`30213af`).
- Added controller auto-configuration from Log Viewer to set rsyslog target (`dee8ece`).
- Addressed environments without mDNS by preferring explicit host/IP behavior (`16ae1b8`).
- Simplified Log Viewer setup to explicit collector IPv4 and port input (`eba7bf2`).
- Added explicit user hint to provide collector host IP in the UI (`8a49cc8`).

### Local Log Collector Service

- Introduced new repository `lightinator-log-service` and scaffolded a local-first collector service (`7330bfd`).
- Added container runtime helper scripts and fixed executable mode for fresh clones (`bf6db1b`).
- Exposed collector IPv4 addresses in service metadata API for frontend integration (`31ef598`).
- Included GHCR multi-arch image publishing workflow and Docker/Podman friendly runtime support.

### User-visible setup flow (current)

- Run the collector service locally (container).
- In the webapp Log Viewer, enter collector IPv4 and HTTP port.
- On refresh, webapp pulls logs from collector API and configures controller rsyslog target to collector IPv4 (UDP 5514).

### Notes

- This release intentionally favors explicit IP-based setup over automatic mDNS discovery to improve reliability across browser and LAN environments.
- Additional discovery and auto-setup enhancements are deferred to later releases.

### Why the extra collector-container step was necessary

- Browsers cannot receive raw UDP syslog directly from controllers, so frontend-only log ingestion is not feasible.
- Keeping a continuous log streaming server on ESP8266-class devices would increase RAM and complexity pressure on constrained firmware.
- A local collector process is the protocol bridge: UDP syslog in, HTTP API out for the webapp.
- Running this as a container makes deployment predictable across Linux hosts (x86_64 and arm64), avoids host dependency drift, and keeps logs local on the user network.
- This architecture also improves privacy and support workflows: logs stay local by default, while still enabling easy export/download when needed.
