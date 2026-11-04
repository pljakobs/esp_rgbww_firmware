import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]
EXPRESSION = re.compile(r"\$\{\{\s*(.*?)\s*\}\}", re.DOTALL)


def workflow(name):
    document = yaml.safe_load((ROOT / ".github/workflows" / name).read_text())
    if True in document and "on" not in document:
        document["on"] = document.pop(True)
    return document


def step(document, job, name):
    return next(value for value in document["jobs"][job]["steps"] if value.get("name") == name)


class WorkflowPolicyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="workflow-policy-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
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

    def test_branch_resolution_for_push_dispatch_and_pr(self):
        document = workflow("build_firmware.yml")
        for job in ("host-smoke", "build"):
            for event, expected in (("push", "testing"), ("workflow_dispatch", "experimental"), ("pull_request", "feature/policy")):
                with self.subTest(job=job, event=event):
                    result = self.execute(step(document, job, "Determine branch"), {
                        "github.event_name": event, "inputs.branch": "experimental",
                        "github.ref_name": "testing", "github.head_ref": "feature/policy",
                    })
                    self.assertEqual(result["branch"], expected)

    def test_pr_branch_is_data_not_shell_code(self):
        document = workflow("build_firmware.yml")
        payload = 'feature/$(touch should-not-exist)'
        for job in ("host-smoke", "build"):
            result = self.execute(step(document, job, "Determine branch"), {
                "github.event_name": "pull_request", "inputs.branch": "",
                "github.ref_name": "123/merge", "github.head_ref": payload,
            })
            self.assertEqual(result["branch"], payload)
            self.assertFalse((self.directory / "should-not-exist").exists())

    def test_current_soc_matrix_and_build_variants(self):
        document = workflow("build_firmware.yml")
        outputs = self.execute(step(document, "generate-matrix", "Define supported SOCs"))
        self.assertEqual(json.loads(outputs["soc_json"]), ["esp8266", "esp32", "esp32c3"])
        self.assertEqual(outputs["soc_csv"], "esp8266,esp32,esp32c3")
        matrix = document["jobs"]["build"]["strategy"]["matrix"]
        self.assertIn("needs.generate-matrix.outputs.soc_json", matrix["soc"])
        self.assertEqual(matrix["release"], [0, 1])
        self.assertEqual(matrix["single"], [0, 1])

    def test_current_branch_mapping_and_slash_safe_artifact_keys(self):
        document = workflow("build_firmware.yml")
        for job in ("host-smoke", "build"):
            for branch in ("develop", "testing", "stable", "experimental"):
                result = self.execute(step(document, job, "Map firmware branch to webapp branch"), {"steps.get_branch.outputs.branch": branch})
                self.assertEqual(result["webapp_branch"], branch)
            result = self.execute(step(document, job, "Sanitize branch name for artifact"), {"steps.get_branch.outputs.branch": "5.1/develop"})
            self.assertEqual(result["sanitized_branch"], "5.1-develop")

    def test_actual_firmware_version_step_with_isolated_tag_fixtures(self):
        selected = step(workflow("build_firmware.yml"), "build", "Get firmware version")
        cases = [
            ("develop", "push", [], "V5.0.1-1-develop"),
            ("develop", "push", ["v5.0.0-9-develop"], "V5.0.0-10-develop"),
            ("experimental", "push", ["v5.0.0-9-develop"], "V5.0.0-10-experimental"),
            ("testing", "push", ["v5.0.2"], "V5.0.3"),
            ("stable", "push", ["v5.0.2"], "V5.1.0"),
            ("develop", "push", ["v5.0.2", "v5.0.3-7-develop"], "V5.0.3-8-develop"),
            ("testing", "pull_request", ["v5.0.2"], "V5.0.2"),
        ]
        for branch, event, tags, expected in cases:
            with self.subTest(branch=branch, event=event, tags=tags):
                result = self.execute(selected, {"steps.sanitize_branch.outputs.sanitized_branch": branch,
                    "steps.get_branch.outputs.branch": branch, "github.event_name": event},
                    {"CI_TEST_TAGS": "\n".join(tags)})
                self.assertEqual(result["fw_version"], expected)
                self.assertEqual(result["fw_build_tag"], "v" + expected[1:])

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

    def test_only_intended_current_branches_trigger_firmware_build(self):
        events = workflow("build_firmware.yml")["on"]
        expected = ["stable", "testing", "develop", "experimental"]
        self.assertEqual(events["push"]["branches"], expected)
        self.assertEqual(events["pull_request"]["branches"], expected)
        self.assertEqual(events["workflow_dispatch"]["inputs"]["branch"]["options"], expected)

    def test_self_hosted_swarm_never_runs_pr_code(self):
        job = workflow("swarm_test.yml")["jobs"]["swarm-mdns"]
        self.assertIn("self-hosted", job["runs-on"])
        self.assertEqual(job["if"], "github.event_name != 'pull_request'")


if __name__ == "__main__":
    unittest.main()