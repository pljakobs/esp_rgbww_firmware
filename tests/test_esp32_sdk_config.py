from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("architecture", ["Esp32", "Esp8266", "Host"])
def test_mailbox_config_is_esp32_only(architecture):
    """Only ESP32-family builds select the 64-entry lwIP mailbox defaults."""
    makefile = (
        f"include {ROOT / 'component.mk'}\n"
        ".PHONY: print-sdk-config\n"
        "print-sdk-config:\n"
        "\t@printf '%s\\n' '$(SDK_CUSTOM_CONFIG)'\n"
    )
    result = subprocess.run(
        ["make", "--no-print-directory", "-f", "-", "print-sdk-config",
         f"PROJECT_DIR={ROOT}", f"SMING_ARCH={architecture}",
         "GIT_VERSION=test", "GIT_DATE=test", "SMING_GITVERSION=test"],
        input=makefile, cwd=ROOT, capture_output=True, text=True, check=True,
    )
    selected = result.stdout.splitlines()[-1]
    if architecture == "Esp32":
        assert selected == "sdkconfig.esp32"
        settings = (ROOT / selected).read_text().splitlines()
        assert "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64" in settings
    else:
        assert selected == ""
