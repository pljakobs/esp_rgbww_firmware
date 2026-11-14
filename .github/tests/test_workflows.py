import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]
EXPRESSION = re.compile(r"\$\{\{\s*(.*?)\s*\}\}", re.DOTALL)
sys.path.insert(0, str(ROOT / ".github" / "scripts"))

from release_metadata import compute_version, resolve_branch


def workflow(name):
    document = yaml.safe_load((ROOT / ".github/workflows" / name).read_text())
    if True in document and "on" not in document:
        document["on"] = document.pop(True)
    return document


def step(document, job, name):
    return next(value for value in document["jobs"][job]["steps"] if value.get("name") == name)


class WorkflowPolicyTests(unittest.TestCase):
    def test_container_dispatch_preserves_exact_source_revision(self):
        document = workflow("build-container.yml")
        self.assertIn("sming_commit", document["on"]["workflow_dispatch"]["inputs"])
        self.assertEqual(document["permissions"], {"contents": "read"})
        self.assertFalse(document["concurrency"]["cancel-in-progress"])
        clone = step(document, "build", "Clone exact Sming source")
        self.assertEqual(clone["env"]["SMING_COMMIT"], "${{ inputs.sming_commit }}")
        self.assertIn('merge-base --is-ancestor "$SMING_COMMIT" origin/develop', clone["run"])
        self.assertIn('checkout --detach "$SMING_COMMIT"', clone["run"])
        build = step(document, "build", "Build and push Docker image")["with"]
        self.assertIn("SMING_COMMIT=${{ env.SMING_COMMIT }}", build["build-args"])
        self.assertIn("org.opencontainers.image.revision=${{ env.SMING_COMMIT }}", build["labels"])
        dockerfile = (ROOT / "docker/Dockerfile").read_text()
        self.assertIn('checkout --detach "$SMING_COMMIT"', dockerfile)
        self.assertIn('test "$(git -C /opt/Sming rev-parse HEAD)" = "$SMING_COMMIT"', dockerfile)
        self.assertNotIn("ESP32_HARDWARE_PWM_COMMIT", dockerfile)
        self.assertIn("git rev-parse HEAD:Sming/Libraries/Esp32HardwarePwm", dockerfile)

    def test_container_dispatch_skips_update_age_check(self):
        selected = step(workflow("build-container.yml"), "check-updates",
                        "Check for updates in pljakobs/Sming")
        result = self.execute(selected, {"github.event_name": "workflow_dispatch"})
        self.assertEqual(result["updated"], "true")

    def test_container_rejects_non_sha_input_without_executing_it(self):
        selected = step(workflow("build-container.yml"), "build", "Clone exact Sming source")
        for value in ("develop", "abc123", "$(touch should-not-exist)", "a" * 39, "A" * 40):
            with self.subTest(value=value):
                self.execute(selected, {"inputs.sming_commit": value}, success=False)
                self.assertFalse((self.directory / "should-not-exist").exists())

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="workflow-policy-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        shutil.copytree(ROOT / ".github" / "scripts", self.directory / ".github" / "scripts")
        tools = self.directory / "tools"
        tools.mkdir()
        git = tools / "git"
        git.write_text("""#!/usr/bin/env python3
import fnmatch
import os
import sys
arguments = sys.argv[1:]
if arguments == ['fetch', '--tags', '--force']:
    raise SystemExit(0)
if arguments[:2] == ['tag', '--list']:
    pattern = arguments[2] if len(arguments) > 2 else '*'
    print('\\n'.join(tag for tag in os.environ.get('CI_TEST_TAGS', '').splitlines() if fnmatch.fnmatchcase(tag, pattern)))
    raise SystemExit(0)
raise SystemExit('Forbidden git operation in policy test: ' + repr(arguments))
""")
        git.chmod(0o755)
        for name in ("curl", "wget", "ssh", "scp", "rsync", "docker", "podman", "sudo"):
            blocked = tools / name
            blocked.write_text("#!/bin/sh\necho 'Deployment/network command forbidden in policy tests' >&2\nexit 97\n")
            blocked.chmod(0o755)
        self.environment = {
            "PATH": str(tools) + os.pathsep + os.environ.get("PATH", os.defpath),
            "HOME": str(self.directory), "GITHUB_OUTPUT": str(self.directory / "output"),
            "GITHUB_ENV": str(self.directory / "environment"), "CI_TEST_TAGS": "",
        }

    def execute(self, selected, values=None, environment=None, success=True):
        values = values or {}

        def render(text):
            return EXPRESSION.sub(lambda match: str(values[match[1].strip()]), str(text))

        source = render(selected["run"])
        configured = {key: render(value) for key, value in selected.get("env", {}).items()}
        output = Path(self.environment["GITHUB_OUTPUT"])
        output.write_text("")
        result = subprocess.run([shutil.which("bash"), "--noprofile", "--norc", "-e"], input=source,
                                cwd=self.directory, env={**self.environment, **configured, **(environment or {})},
                                capture_output=True, text=True, timeout=10)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return dict(line.split("=", 1) for line in output.read_text().splitlines() if "=" in line)

    def test_every_workflow_and_inline_shell_parses(self):
        paths = sorted((ROOT / ".github/workflows").iterdir())
        for path in paths:
            if path.suffix not in {".yml", ".yaml"}:
                continue
            document = yaml.safe_load(path.read_text())
            self.assertIn("jobs", document, str(path))
            for job in document["jobs"].values():
                for selected in job.get("steps", []):
                    if "run" not in selected:
                        continue
                    with self.subTest(workflow=path.name, step=selected.get("name")):
                        source = EXPRESSION.sub("CI_EXPRESSION", selected["run"])
                        result = subprocess.run(["bash", "-n"], input=source, capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stderr)

    def test_validation_is_unprivileged_and_runs_for_every_pr(self):
        document = workflow("validate-workflows.yml")
        events = document.get("on", document.get(True))
        self.assertIn("pull_request", events)
        self.assertFalse(events["pull_request"])
        self.assertEqual(document["permissions"], {"contents": "read"})
        self.assertNotIn("secrets.", json.dumps(document))
        self.assertEqual(document["jobs"]["validate"]["runs-on"], "ubuntu-latest")
        self.assertFalse(step(document, "validate", "Checkout")["with"]["persist-credentials"])


    def test_validation_sets_up_node_before_python_tests(self):
        steps = workflow("validate-workflows.yml")["jobs"]["validate"]["steps"]
        names = [selected.get("name") for selected in steps]
        self.assertLess(names.index("Set up Node.js"), names.index("Test workflow syntax and policies"))


    def test_host_smoke_runs_sensor_and_transport_regressions(self):
        script = (ROOT / ".github/scripts/host_ci_smoke_test.sh").read_text()
        for name in ("test_as7341_measurement.py", "test_ina219_measurement.py",
                     "test_extension_schema_composition.py", "websocket_output_test.py"):
            self.assertIn(f"tests/{name}", script)

    def test_branch_resolution_for_push_dispatch_and_pr(self):
        document = workflow("build_firmware.yml")
        selected = step(document, "generate-matrix", "Resolve release matrix")
        cases = [
            ({"github.event_name": "push", "inputs.branch": "", "github.ref_name": "5.1/develop",
              "github.head_ref": "", "github.base_ref": "", "github.sha": "push-sha",
              "steps.manual_source.outputs.sha": ""}, "5.1/develop", "5.1/develop", "push-sha"),
            ({"github.event_name": "workflow_dispatch", "inputs.branch": "5.0/testing", "github.ref_name": "",
              "github.head_ref": "", "github.base_ref": "", "github.sha": "workflow-sha",
              "steps.manual_source.outputs.sha": "selected-sha"}, "5.0/testing", "5.0/testing", "selected-sha"),
            ({"github.event_name": "pull_request", "inputs.branch": "", "github.ref_name": "feature/policy",
              "github.head_ref": "feature/policy", "github.base_ref": "5.1/develop",
              "github.sha": "pr-merge-sha", "steps.manual_source.outputs.sha": ""},
             "feature/policy", "5.1/develop", "pr-merge-sha"),
        ]
        for values, source_ref, release_ref, checkout_ref in cases:
            with self.subTest(event=values["github.event_name"]):
                result = self.execute(selected, values)
                self.assertEqual(result["source_ref"], source_ref)
                self.assertEqual(result["release_ref"], release_ref)
                self.assertEqual(result["checkout_ref"], checkout_ref)
                self.assertEqual(result["release_line"], release_ref[:3])

    def test_build_jobs_checkout_the_resolved_immutable_sha(self):
        document = workflow("build_firmware.yml")
        expected = "${{ needs.generate-matrix.outputs.checkout_ref }}"
        self.assertEqual(step(document, "host-smoke", "Checkout Repository")["with"]["ref"], expected)
        self.assertEqual(step(document, "build", "Checkout Repository")["with"]["ref"], expected)
        manual_checkout = step(document, "generate-matrix", "Checkout selected manual source")["with"]
        self.assertEqual(manual_checkout["ref"], "${{ inputs.branch }}")
        self.assertFalse(manual_checkout["persist-credentials"])

    def test_standalone_host_smoke_uses_resolved_sha_and_valid_toolchain_path(self):
        document = workflow("build_firmware.yml")
        job = document["jobs"]["host-smoke-test"]
        self.assertIn("generate-matrix", job["needs"])
        checkout = step(document, "host-smoke-test", "Checkout Repository")["with"]
        self.assertEqual(checkout["ref"], "${{ needs.generate-matrix.outputs.checkout_ref }}")
        source = step(document, "host-smoke-test", "Test HTTP API via Host Emulator")["run"]
        self.assertIn("/opt/Sming/Tools/export.sh", source)
        self.assertNotIn("/opt/Sning/Tools/export.sh", source)

    def test_pr_branch_is_data_not_shell_code(self):
        document = workflow("build_firmware.yml")
        payload = 'feature/$(touch should-not-exist)'
        result = self.execute(step(document, "generate-matrix", "Resolve release matrix"), {
            "github.event_name": "pull_request", "inputs.branch": "",
            "github.ref_name": "123/merge", "github.head_ref": payload,
            "github.base_ref": "5.0/develop", "github.sha": "pr-merge-sha",
            "steps.manual_source.outputs.sha": "",
        })
        self.assertEqual(result["source_ref"], payload)
        self.assertEqual(result["release_ref"], "5.0/develop")
        self.assertFalse((self.directory / "should-not-exist").exists())

    def test_release_refs_are_passed_as_environment_data_to_shell(self):
        document = workflow("build_firmware.yml")
        checks = [
            ("host-smoke", "Create CI build tag", "CI_SOURCE_REF"),
            ("host-smoke", "Prepare host-smoke artifact directory", "CI_SOURCE_REF"),
            ("build", "Notify webapp of schema update", "CI_FIRMWARE_BRANCH"),
        ]
        for job, name, key in checks:
            with self.subTest(step=name):
                selected = step(document, job, name)
                self.assertIn(key, selected.get("env", {}))
                self.assertNotRegex(selected["run"], r"\$\{\{[^}]*outputs\.(source_ref|release_ref)")

    def test_current_soc_matrix_and_build_variants(self):
        document = workflow("build_firmware.yml")
        selected = step(document, "generate-matrix", "Resolve release matrix")
        outputs = self.execute(selected, {
            "github.event_name": "push", "inputs.branch": "", "github.ref_name": "5.1/develop",
            "github.head_ref": "", "github.base_ref": "", "github.sha": "push-sha",
            "steps.manual_source.outputs.sha": "",
        })
        self.assertEqual(json.loads(outputs["soc_json"]), ["esp32", "esp32c3"])
        self.assertEqual(outputs["soc_csv"], "esp32,esp32c3")
        self.assertEqual(outputs["release_ref"], "5.1/develop")
        self.assertEqual(outputs["release_line"], "5.1")
        self.assertEqual(outputs["stage"], "develop")
        self.assertEqual(outputs["rgbwwled_version"], "0.11.0")
        self.assertNotIn("upgrade_from_lines", outputs)
        matrix = document["jobs"]["build"]["strategy"]["matrix"]
        self.assertIn("needs.generate-matrix.outputs.soc_json", matrix["soc"])
        self.assertEqual(matrix["release"], [0, 1])
        self.assertEqual(matrix["single"], [0, 1])

    def test_build_verifies_the_line_specific_rgbwwled_source_version(self):
        document = workflow("build_firmware.yml")
        resolver = step(document, "generate-matrix", "Resolve release matrix")
        component_check = step(document, "build", "Verify RGBWWLed release pin")
        self.assertIn("steps.resolve.outputs.rgbwwled_version", json.dumps(document["jobs"]["generate-matrix"]["outputs"]))
        self.assertIn("needs.generate-matrix.outputs.rgbwwled_version", component_check["env"]["EXPECTED_RGBWWLED_VERSION"])
        self.assertIn("Components/RGBWWLed/RGBWWconst.h", component_check["run"])
        self.assertIn("$EXPECTED_RGBWWLED_VERSION", component_check["run"])
        for branch, version in (("5.0/develop", "0.10.1"), ("5.1/develop", "0.11.0")):
            with self.subTest(branch=branch):
                result = self.execute(resolver, {
                    "github.event_name": "push", "inputs.branch": "", "github.ref_name": branch,
                    "github.head_ref": "", "github.base_ref": "", "github.sha": "push-sha",
                    "steps.manual_source.outputs.sha": "",
                })
                self.assertEqual(result["rgbwwled_version"], version)

    def test_all_builds_keep_the_firmware_bootstrap_and_externalize_the_spa(self):
        document = workflow("build_firmware.yml")
        for job in ("host-smoke", "build"):
            names = {selected.get("name") for selected in document["jobs"][job]["steps"]}
            self.assertNotIn("Download fileList.h artifact", names)
            self.assertNotIn("Download webapp files artifact", names)
        firmware_files = (ROOT / "include" / "fileList.h").read_text()
        self.assertIn('"captive.html"', firmware_files)
        self.assertIn('"updating.html"', firmware_files)
        self.assertNotIn("spa-files.zip", json.dumps(document["jobs"]["build"]))

    def test_versioned_refs_separate_publisher_identity_from_artifact_names(self):
        cases = {
            "5.0/develop": ("5.0/develop", "5.0-develop", "5.0", "develop", ["esp8266", "esp32", "esp32c3"], "0.10.1"),
            "5.1/develop": ("5.1/develop", "5.1-develop", "5.1", "develop", ["esp32", "esp32c3"], "0.11.0"),
            "5.1/testing": ("5.1/testing", "5.1-testing", "5.1", "testing", ["esp32", "esp32c3"], "0.11.0"),
            "5.0/prod": ("5.0/prod", "5.0-prod", "5.0", "prod", ["esp8266", "esp32", "esp32c3"], "0.10.1"),
        }
        for branch, expected in cases.items():
            with self.subTest(branch=branch):
                metadata = resolve_branch(branch)
                self.assertEqual(
                    (metadata["release_ref"], metadata["artifact_branch"], metadata["release_line"],
                     metadata["stage"], list(metadata["socs"]), metadata["rgbwwled_version"]),
                    expected,
                )
                self.assertEqual(json.loads(metadata["soc_json"]), expected[4])

    def test_release_metadata_does_not_require_upgrade_qualification(self):
        for branch in ("5.0/develop", "5.1/develop"):
            self.assertNotIn("upgrade_from_lines", resolve_branch(branch))

    def test_unversioned_refs_are_rejected(self):
        for branch in ("develop", "testing", "stable", "experimental"):
            with self.subTest(branch=branch):
                with self.assertRaises(ValueError):
                    resolve_branch(branch)

    def test_unknown_and_mismatched_release_refs_are_rejected(self):
        for branch in ("feature/policy", "5.0/experimental", "5.2/develop", "5.1//develop"):
            with self.subTest(branch=branch), self.assertRaises(ValueError):
                resolve_branch(branch)

    def test_firmware_version_counters_are_independent_per_release_line(self):
        tags = ["v5.0.0-1019-develop", "v5.0.0-1014-experimental", "v5.1.0-5-develop"]
        self.assertEqual(compute_version(tags, "5.0", "develop"), {
            "fw_version": "V5.0.0-1020-develop", "fw_build_tag": "v5.0.0-1020-develop",
        })
        self.assertEqual(compute_version(tags, "5.1", "testing"), {
            "fw_version": "V5.1.0-6-testing", "fw_build_tag": "v5.1.0-6-testing",
        })
        self.assertEqual(compute_version([], "5.1", "develop"), {
            "fw_version": "V5.1.0-1-develop", "fw_build_tag": "v5.1.0-1-develop",
        })
        self.assertEqual(compute_version(["v5.0.2"], "5.0", "testing"), {
            "fw_version": "V5.0.3-1-testing", "fw_build_tag": "v5.0.3-1-testing",
        })
        self.assertEqual(compute_version(["v5.0.2", "v5.0.3-7-develop"], "5.0", "develop"), {
            "fw_version": "V5.0.3-8-develop", "fw_build_tag": "v5.0.3-8-develop",
        })
        self.assertEqual(compute_version(["v5.0.3"], "5.0", "prod"), {
            "fw_version": "V5.0.4-1-prod", "fw_build_tag": "v5.0.4-1-prod",
        })

    def test_current_branch_mapping_and_slash_safe_artifact_keys(self):
        for branch, artifact_name in (("5.0/develop", "5.0-develop"), ("5.1/develop", "5.1-develop")):
            with self.subTest(branch=branch):
                result = resolve_branch(branch)
                self.assertEqual(result["artifact_branch"], artifact_name)

    def test_actual_firmware_version_step_with_isolated_tag_fixtures(self):
        selected = step(workflow("build_firmware.yml"), "build", "Get firmware version")
        cases = [
            ("5.0", "develop", ["v5.0.0-1019-develop"], "V5.0.0-1020-develop"),
            ("5.0", "develop", ["v5.0.0-1014-experimental", "v5.0.0-1019-develop"], "V5.0.0-1020-develop"),
            ("5.1", "develop", ["v5.0.0-1019-develop"], "V5.1.0-1-develop"),
            ("5.1", "develop", ["v5.1.0-9-develop"], "V5.1.0-10-develop"),
            ("5.1", "testing", ["v5.0.0-1019-develop"], "V5.1.0-1-testing"),
            ("5.0", "prod", ["v5.0.0-1019-develop"], "V5.0.0-1020-prod"),
        ]
        for release_line, stage, tags, expected in cases:
            with self.subTest(release_line=release_line, stage=stage, tags=tags):
                result = self.execute(selected, {
                    "needs.generate-matrix.outputs.release_line": release_line,
                    "needs.generate-matrix.outputs.stage": stage,
                    "needs.generate-matrix.outputs.release_ref": f"{release_line}/{stage}",
                },
                    {"CI_TEST_TAGS": "\n".join(tags)})
                self.assertEqual(result["fw_version"], expected)
                self.assertEqual(result["fw_build_tag"], "v" + expected[1:])
                self.assertEqual(result["fw_branch"], f"{release_line}/{stage}")

    def test_publisher_requires_immutable_pin(self):
        document = workflow("deploy-pages.yml")
        selected = step(document, "publish", "Validate publisher pin")
        for invalid in ("", "main", "f" * 39, "F" * 40, "f" * 41):
            with self.subTest(pin=invalid):
                self.execute(selected, environment={"DISTRIBUTION_PUBLISHER_SHA": invalid}, success=False)
        self.execute(selected, environment={"DISTRIBUTION_PUBLISHER_SHA": "f" * 40})
        checkout = step(document, "publish", "Checkout pinned distribution publisher")["with"]
        self.assertEqual(checkout["ref"], "${{ env.DISTRIBUTION_PUBLISHER_SHA }}")
        self.assertFalse(checkout["persist-credentials"])

    def test_publisher_bundle_is_run_scoped_and_tests_the_pinned_source(self):
        document = workflow("deploy-pages.yml")
        publish = document["jobs"]["publish"]
        names = {selected.get("name") for selected in publish["steps"]}
        self.assertIn("Validate publisher pin", names)
        self.assertIn("Checkout pinned distribution publisher", names)
        self.assertIn("Test pinned distribution publisher", names)
        text = json.dumps(publish)
        self.assertIn("publisher-firmware-${{ env.deploy_run_key }}", text)
        self.assertIn("SOURCE_REVISION", text)
        self.assertIn("requirements-test.txt", text)
        self.assertIn("manage_version.py --publisher-contract", text)

    def test_firmware_publisher_uses_canonical_ref_and_artifact_safe_key(self):
        build = workflow("build_firmware.yml")
        deploy = workflow("deploy-pages.yml")
        resolved = self.execute(step(build, "generate-matrix", "Resolve release matrix"), {
            "github.event_name": "push", "inputs.branch": "", "github.ref_name": "5.1/testing",
            "github.head_ref": "", "github.base_ref": "", "github.sha": "push-sha",
            "steps.manual_source.outputs.sha": "",
        })
        self.assertEqual(resolved["release_ref"], "5.1/testing")
        self.assertEqual(resolved["artifact_branch"], "5.1-testing")
        version_name = step(deploy, "get-version", "Download version info")["with"]["name"]
        self.assertIn("steps.get_branch.outputs.artifact_branch", version_name)
        setup = step(deploy, "publish", "Setup branch variables")["run"]
        self.assertIn('source_branch="${{ needs.get-version.outputs.artifact_branch }}"', setup)
        self.assertIn('target_branch="${{ needs.get-version.outputs.fw_branch }}"', setup)
        remote_script = step(deploy, "publish", "Deploy to External Server (rsync)")["run"]
        self.assertIn('version.json add "$soc" "$type" "$TARGET_BRANCH" "$VERSION"', remote_script)
        self.assertNotIn("--upgrade-from-lines", remote_script)

    def test_firmware_publisher_never_publishes_pr_builds(self):
        document = workflow("deploy-pages.yml")
        cases = []
        for event_name in ("workflow_dispatch", "workflow_run", "pull_request"):
            for source_event in ("push", "workflow_dispatch", "pull_request", "pull_request_target"):
                for conclusion in ("success", "failure", "cancelled", "skipped"):
                    for repository in ("owner/firmware", "fork/firmware"):
                        github = {
                            "event_name": event_name,
                            "repository": "owner/firmware",
                            "event": {"workflow_run": {
                                "event": source_event if event_name == "workflow_run" else "",
                                "conclusion": conclusion if event_name == "workflow_run" else "",
                                "head_repository": {
                                    "full_name": repository if event_name == "workflow_run" else "",
                                },
                            }},
                        }
                        expected = event_name == "workflow_dispatch" or (
                            event_name == "workflow_run" and conclusion == "success"
                            and source_event in ("push", "workflow_dispatch")
                            and repository == "owner/firmware"
                        )
                        cases.append({"github": github, "expected": expected})
        script = """
const { readFileSync } = require("node:fs");
const { runInNewContext } = require("node:vm");
const { conditions, cases } = JSON.parse(readFileSync(0, "utf8"));
for (const [job, condition] of Object.entries(conditions)) {
    for (const { github, expected } of cases) {
        const actual = runInNewContext(condition, {
            github, fromJSON: JSON.parse,
            contains: (values, value) => values.includes(value),
        }, { timeout: 1000 });
        if (actual !== expected) {
            throw new Error(JSON.stringify({ job, github, expected, actual }));
        }
    }
}
"""
        conditions = {
            job: EXPRESSION.fullmatch(document["jobs"][job]["if"]).group(1)
            for job in ("get-version", "publish")
        }
        result = subprocess.run(["node", "-e", script],
                                input=json.dumps({"conditions": conditions, "cases": cases}),
                                env=self.environment, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        events = document["on"]
        self.assertEqual(events["workflow_run"]["branches"][-2:], ["5.0/**", "5.1/**"])
        options = events["workflow_dispatch"]["inputs"]["branch"]["options"]
        self.assertEqual(options[:6], ["5.0/develop", "5.0/testing", "5.0/prod", "5.1/develop", "5.1/testing", "5.1/prod"])

    def test_publisher_resolves_refs_without_checkout_of_triggering_source(self):
        document = workflow("deploy-pages.yml")
        resolve = step(document, "get-version", "Get Branch and Artifact Branch")
        self.assertNotIn("Checkout release metadata helper", [
            selected.get("name") for selected in document["jobs"]["get-version"]["steps"]
        ])
        cases = [
            ("workflow_run", "", "5.1/develop", "5.1/develop", "5.1-develop"),
            ("workflow_dispatch", "5.0/prod", "", "5.0/prod", "5.0-prod"),
            ("workflow_dispatch", "5.1/develop", "", "5.1/develop", "5.1-develop"),
        ]
        for event_name, input_branch, run_branch, release_ref, artifact_branch in cases:
            with self.subTest(event=event_name, branch=input_branch or run_branch):
                result = self.execute(resolve, {
                    "github.event_name": event_name,
                    "inputs.branch": input_branch,
                    "github.event.inputs.branch": input_branch,
                    "github.event.workflow_run.head_branch": run_branch,
                })
                self.assertEqual(result["release_ref"], release_ref)
                self.assertEqual(result["artifact_branch"], artifact_branch)
        with self.subTest(event="workflow_run", branch="feature/untrusted"):
            self.execute(resolve, {
                "github.event_name": "workflow_run", "inputs.branch": "",
                "github.event.inputs.branch": "",
                "github.event.workflow_run.head_branch": "feature/untrusted",
            }, success=False)
        with self.subTest(event="workflow_dispatch", branch="develop"):
            self.execute(resolve, {
                "github.event_name": "workflow_dispatch", "inputs.branch": "develop",
                "github.event.inputs.branch": "develop",
                "github.event.workflow_run.head_branch": "",
            }, success=False)

    def test_version_info_artifact_is_validated_as_data_not_sourced(self):
        document = workflow("deploy-pages.yml")
        load_info = step(document, "get-version", "Load version info")
        version_directory = self.directory / "version-info"
        version_directory.mkdir()
        cases = [
            ("5.0/develop", "V5.0.2-7-develop", "esp8266,esp32,esp32c3", "0.10.1", ""),
            ("5.1/develop", "V5.1.0-2-develop", "esp32,esp32c3", "0.11.0", ""),
            ("5.1/develop", "V5.1.0-2-develop", "esp32,esp32c3", "0.11.0", "upgrade_from_lines=5.0\n"),
        ]
        for release_ref, version, socs, component_version, legacy_metadata in cases:
            with self.subTest(release_ref=release_ref):
                (version_directory / "version-info.txt").write_text(
                    f"fw_version={version}\nfw_branch={release_ref}\nfw_comment_b64=\nsupported_socs={socs}\nrgbwwled_version={component_version}\n{legacy_metadata}"
                )
                result = self.execute(load_info, {
                    "steps.get_branch.outputs.release_ref": release_ref,
                })
                self.assertEqual(result["fw_version"], version)
                self.assertEqual(result["fw_branch"], release_ref)
                self.assertEqual(result["supported_socs"], socs)
                self.assertNotIn("upgrade_from_lines", result)

        marker = self.directory / "artifact-command-ran"
        (version_directory / "version-info.txt").write_text(
            "fw_version=V5.1.0-2-develop\nfw_branch=5.1/develop\n"
            f"fw_comment_b64=$(touch {marker})\nsupported_socs=esp32,esp32c3\n"
            "rgbwwled_version=0.11.0\nupgrade_from_lines=5.0\n"
        )
        self.execute(load_info, {
            "steps.get_branch.outputs.release_ref": "5.1/develop",
        }, success=False)
        self.assertFalse(marker.exists())

    def test_only_intended_current_branches_trigger_firmware_build(self):
        events = workflow("build_firmware.yml")["on"]
        canonical = ["5.0/**", "5.1/**"]
        self.assertEqual(events["push"]["branches"], canonical)
        self.assertEqual(events["pull_request"]["branches"], canonical)
        options = events["workflow_dispatch"]["inputs"]["branch"]["options"]
        self.assertEqual(options, ["5.0/develop", "5.0/testing", "5.0/prod", "5.1/develop", "5.1/testing", "5.1/prod"])

    def test_self_hosted_swarm_never_runs_pr_code(self):
        document = workflow("swarm_test.yml")
        job = document["jobs"]["swarm-mdns"]
        self.assertIn("self-hosted", job["runs-on"])
        self.assertEqual(job["if"], "github.event_name != 'pull_request'")
        expected_lines = ["5.0/**", "5.1/**"]
        self.assertEqual(document["on"]["push"]["branches"], expected_lines)
        self.assertEqual(document["on"]["pull_request"]["branches"], expected_lines)
        self.assertEqual(document["on"]["workflow_dispatch"]["inputs"]["branch"]["options"],
                 ["5.0/develop", "5.0/testing", "5.0/prod", "5.1/develop", "5.1/testing", "5.1/prod"])
        checkout = step(document, "swarm-mdns", "Checkout Repository")["with"]
        self.assertEqual(checkout["ref"], "${{ inputs.branch || github.ref }}")


if __name__ == "__main__":
    unittest.main()