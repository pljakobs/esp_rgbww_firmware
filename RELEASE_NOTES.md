# Release Notes

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
