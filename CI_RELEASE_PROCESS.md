# CI, Build, and Release Process

This document describes the versioned firmware/webapp CI preparation currently on `ci/versioned-firmware-publishing` and `ci/versioned-webapp-publishing`. The workflow changes are pushed and their workflow-validation checks pass, but the changes are not merged and the `5.0/*` and `5.1/*` release branches have not been cut over. Legacy branch aliases remain enabled during migration.

This is the operational process implemented by the current workflows, not the later signed-manifest or build-once promotion design. The production publisher remains the Milestone 1 pinned `manage_version.py` bundle over SSH/rsync; no HTTP publishing is used.

## Release Identity

The canonical ref is `<release-line>/<stage>`. The release line and stage are distinct: `5.0` and `5.1` are lines; `develop`, `testing`, and `prod` are stages.

| Legacy ref | Canonical ref | RGBWWLed | SoCs |
| --- | --- | --- | --- |
| `develop` | `5.0/develop` | `0.10.1` | ESP8266, ESP32, ESP32-C3 |
| `testing` | `5.0/testing` | `0.10.1` | ESP8266, ESP32, ESP32-C3 |
| `stable` | `5.0/prod` | `0.10.1` | ESP8266, ESP32, ESP32-C3 |
| `experimental` | `5.1/develop` | `0.11.0` | ESP32, ESP32-C3 |
| `5.1/testing` | `5.1/testing` | `0.11.0` | ESP32, ESP32-C3 |
| `5.1/prod` | `5.1/prod` | `0.11.0` | ESP32, ESP32-C3 |

ESP8266 is never built or advertised for 5.1. The CI resolver rejects unknown refs. `stable` is a temporary alias for 5.0 production; remove aliases only after refs, consumers, and deployed artifacts are accounted for.

## Firmware Build

`build_firmware.yml` runs for pushes and pull requests targeting legacy refs or `5.0/**` and `5.1/**`. Manual dispatch accepts the canonical refs and legacy aliases.

The `generate-matrix` job resolves the source ref, canonical release ref, line, stage, RGBWWLed version, SoC list, and artifact-safe key. Push builds use the event commit SHA. Pull requests use the PR merge SHA for checkout and the base branch for release-line policy. Manual dispatch resolves the selected branch to a commit SHA before the host-smoke and matrix jobs check it out.

The matrix preserves debug/release and ordinary/single-image variants. Before compiling, CI checks the pinned submodule's `RGBWW_VERSION` header against the line's required version. The ESP8266 partition map remains ESP8266-only. Once-per-build version metadata, release tagging, and webapp schema notification use an ESP32 matrix cell so they also run on 5.1.

Firmware version tags are scoped by release line. The current format is `V<line>.<patch>-<sequence>-<stage>`, for example `V5.0.0-1020-develop`; the Git tag is the lowercase equivalent. The helper uses existing clean and prerelease tags for that line to choose the next patch and sequence. A clean patch tag advances that line's next prerelease patch. The 5.0 and 5.1 counters do not affect one another.

Artifact names replace `/` in the canonical ref with `-` (for example `5.1/develop` becomes `5.1-develop`). This is only an artifact key. Version metadata and deployed storage paths retain the canonical slash ref.

## External Webapp

The complete SPA is not embedded in firmware. Firmware retains only the small FSTRINGS bootstrap pages listed in `include/fileList.h` (`captive.html` and `updating.html`); the complete webapp is fetched from the distribution server after boot. This applies to both release lines.

The webapp `test.yml` runs tests on legacy and versioned branch pushes/PRs. The deploy workflow builds the branch's corresponding `V5.0` or `V5.1` webapp version, uploads the complete generated SPA asset set, and publishes it under its canonical ref, such as `download/webui/5.1/develop/<version>`. Webapp updates are independently versioned and filtered by firmware compatibility; publishing a V5.0 webapp does not make it eligible for 5.1 firmware.

## Firmware Publication

`deploy-pages.yml` starts after `firmwareBuild` completes, but proceeds only for successful same-repository push runs. PR builds cannot reach the SSH publisher. Manual dispatch selects a canonical/legacy ref and looks up the latest successful push build for that ref.

The publisher downloads version-info and build artifacts from the selected run. It uses the artifact-safe key for GitHub artifact lookup and the canonical release ref for deployed paths and the SQLite record. It then tests the pinned distribution-server revision, stages a run-specific publisher bundle, and uses the existing SSH/rsync process to upload firmware and invoke `manage_version.py` on the server. `DISTRIBUTION_PUBLISHER_SHA` must be a reviewed 40-character commit SHA. `DEPLOY_KEY` is required; the port-knock secret is optional. Firmware-to-webapp schema dispatch uses `WEBAPP_DISPATCH_TOKEN` and is non-fatal if unavailable.

The publisher writes firmware entries to the M1 SQLite catalogue and keeps the legacy JSON projection behavior. It does not expose a new HTTP publishing endpoint. Firmware and webapp publishers use separate run-scoped bundles and remote deployment locks.

## Promotion and Manual Operation

The current CI builds separately on each stage branch. Merging/cherry-picking a candidate into another stage causes a new build and version; it does not promote the exact same binary. Build-once promotion of immutable artifacts is a later release-process improvement, not a current guarantee.

For normal operation, push a tested commit to the intended canonical branch and inspect both build and publish runs. A manual firmware build does not automatically become the input to the manual publisher: the publisher's manual lookup selects a successful push run. Verify the selected run, canonical ref, version, supported SoCs, and publisher SHA before a manual publication.

Do not treat a successful workflow-validation run as proof that a firmware binary or production deployment succeeded. The validation workflow runs actionlint and policy tests only; it performs no firmware build or deployment.

## Reproducibility Status and Gaps

The workflow is traceable, but it does not yet guarantee byte-identical rebuilds. The source commit and RGBWWLed gitlink are pinned for a build, and the component version is checked. However:

- Firmware jobs use `docker.io/pjakobs/sming:latest`, a mutable builder image.
- GitHub-hosted jobs use `ubuntu-latest`; actions are referenced by version tags rather than immutable action SHAs.
- Firmware version/tag selection reads the live Git tag set. Re-running the same source later can produce a different version; concurrent same-line runs can race for the same next sequence.
- The webapp version embeds the GitHub run number, so separate runs intentionally produce different webapp bytes/version metadata.
- The publisher currently logs and continues when an expected artifact is absent; a successful publish is not yet a strict proof that every expected SoC/build variant was present.
- Current M1 publishing records legacy firmware metadata; it does not yet provide the full source/dependency provenance and immutable release-manifest lifecycle described in the later product-aware plan.

Before claiming reproducible or complete releases, pin the builder image by digest and action revisions, use a controlled runner/toolchain, make version allocation collision-safe and explicit, and make publication fail closed unless every expected artifact is present and verified. Build-once promotion should then promote those exact artifacts instead of rebuilding each stage.

## Related Workflows and Plans

- [Firmware build](.github/workflows/build_firmware.yml)
- [Firmware publisher](.github/workflows/deploy-pages.yml)
- [Swarm test](.github/workflows/swarm_test.yml)
- [Webapp test](../esp_rgb_webapp2/.github/workflows/test.yml)
- [Webapp build and publisher](../esp_rgb_webapp2/.github/workflows/deploy.yml)
- [Cross-project M2 plan](../esp_rgb_webapp2/MASTER_PLAN.md)
