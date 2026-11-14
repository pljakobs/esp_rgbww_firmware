from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("architecture,soc,config,core", [
    ("Esp32", "esp32", "sdkconfig.dualcore", "1"),
    ("Esp32", "esp32c3", "sdkconfig.esp32", ""),
    ("Esp32", "esp32s3", "sdkconfig.esp32", ""),
    ("Esp8266", "esp8266", "", ""),
    ("Host", "host", "", ""),
])
def test_mailbox_config_is_esp32_only(architecture, soc, config, core):
    """Only ESP32-family builds select the 64-entry lwIP mailbox defaults."""
    makefile = (
        f"include {ROOT / 'component.mk'}\n"
        ".PHONY: print-sdk-config\n"
        "print-sdk-config:\n"
        "\t@printf 'selected=%s;core=%s\\n' '$(SDK_CUSTOM_CONFIG)' '$(LED_WORKER_CORE)'\n"
    )
    result = subprocess.run(
        ["make", "--no-print-directory", "-f", "-", "print-sdk-config",
         f"PROJECT_DIR={ROOT}", f"SMING_ARCH={architecture}",
         f"SMING_SOC={soc}",
         "GIT_VERSION=test", "GIT_DATE=test", "SMING_GITVERSION=test"],
        input=makefile, cwd=ROOT, capture_output=True, text=True, check=True,
    )
    assert result.stdout.splitlines()[-1] == f"selected={config};core={core}"
    if architecture == "Esp32":
        settings = (ROOT / config).read_text().splitlines()
        assert "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64" in settings
        if soc == "esp32":
            assert "CONFIG_FREERTOS_UNICORE=n" in settings
            assert "CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y" in settings
            assert "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y" in settings
