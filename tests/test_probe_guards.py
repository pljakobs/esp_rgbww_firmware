import pathlib
import subprocess

import pytest


ROOT = pathlib.Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("release", [False, True])
def test_disabled_probe_macros_do_not_evaluate_arguments(tmp_path, release):
    source = tmp_path / "guards.cpp"
    source.write_text("""
#include "CallbackTiming.h"
#include "FadeTiming.h"
#include "EventQueueMonitor.h"
int main()
{
    CALLBACK_TIMING_SCOPE(scope, NotAnActualId);
    CALLBACK_TIMING_START();
    EVENT_QUEUE_MONITOR_START();
    FADE_TIMING_ONLY(undefined_function();)
    FADE_TIMING_LIGHT(undefined_light, undefined_function());
    const auto sample = FADE_TIMING_SAMPLE(undefined_light, undefined_event);
    const auto health = EventQueueMonitor::snapshot();
    return sample || health.available || health.probes;
}
""")
    output = tmp_path / "guards"
    subprocess.run([
        "g++", "-std=c++17", "-DARCH_ESP32",
        f"-DEVENT_MONITOR={int(release)}", "-DFADE_TIMING_DIAGNOSTICS=0",
        *(["-DSMING_RELEASE"] if release else []),
        "-I", str(ROOT / "include"), "-I", str(ROOT / "app"),
        str(source), "-o", str(output),
    ], check=True, capture_output=True, text=True)
    subprocess.run([str(output)], check=True)
