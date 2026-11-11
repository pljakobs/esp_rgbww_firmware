"""Compile the actual ESP32 dispatcher with native FreeRTOS/lwIP test doubles."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


STUBS = {
    "c_types.h": "#pragma once\n#include <cstdint>\n",
    "debug_progmem.h": """
#pragma once
#include <cassert>
#define debug_e(...) (++diagnosticErrors)
#define debug_w(...) (++diagnosticErrors)
inline unsigned diagnosticErrors;
""",
    "esp_timer.h": """
#pragma once
#include <cstdint>
inline int64_t testClock;
inline int64_t esp_timer_get_time() { return testClock; }
""",
    "esp_task.h": """
#pragma once
#include <cassert>
#include <deque>
#define DRAM_ATTR
#define IRAM_ATTR
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
inline unsigned lockDepth;
#define portENTER_CRITICAL(mux) ((void)(mux), ++lockDepth)
#define portEXIT_CRITICAL(mux) ((void)(mux), --lockDepth)
#define portENTER_CRITICAL_ISR(mux) ((void)(mux), ++lockDepth)
#define portEXIT_CRITICAL_ISR(mux) ((void)(mux), --lockDepth)
#define portYIELD_FROM_ISR_ARG(woken) ((void)(woken))
using BaseType_t = int;
constexpr int pdTRUE = 1, pdFALSE = 0, portMAX_DELAY = -1;
constexpr int CONFIG_LWIP_TCPIP_TASK_STACK_SIZE = 16384, CONFIG_LWIP_TCPIP_TASK_PRIO = 18;
struct TestQueue { std::deque<os_event_t> entries; unsigned capacity; };
using QueueHandle_t = TestQueue*;
inline QueueHandle_t xQueueCreate(unsigned count, unsigned) {
    static TestQueue queue;
    queue.capacity = count;
    return &queue;
}
inline unsigned uxQueueMessagesWaiting(QueueHandle_t queue) { return queue->entries.size(); }
inline unsigned uxQueueMessagesWaitingFromISR(QueueHandle_t queue) { return queue->entries.size(); }
inline int xQueueIsQueueEmptyFromISR(QueueHandle_t queue) { return queue->entries.empty(); }
inline int xQueueReceive(QueueHandle_t queue, os_event_t* evt, int) {
    if(queue->entries.empty()) return pdFALSE;
    *evt = queue->entries.front();
    queue->entries.pop_front();
    return pdTRUE;
}
inline int xQueueSendToBackFromISR(QueueHandle_t queue, const os_event_t* evt, int* woken) {
    *woken = 0;
    if(queue->entries.size() == queue->capacity) return pdFALSE;
    queue->entries.push_back(*evt);
    return pdTRUE;
}
inline void xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int, void*, unsigned) {}
""",
    "lwip/tcpip.h": """
#pragma once
using tcpip_callback_fn = void (*)(void*);
struct tcpip_callback_msg { tcpip_callback_fn fn; void* arg; };
constexpr int ERR_OK = 0, ERR_MEM = -1, ERR_NEED_SCHED = 123;
inline int nextPostResult = ERR_OK;
inline unsigned postAttempts;
inline tcpip_callback_msg* tcpip_callbackmsg_new(tcpip_callback_fn fn, void* arg) {
    static tcpip_callback_msg msg{fn, arg};
    return &msg;
}
inline int tcpip_callbackmsg_trycallback_fromisr(tcpip_callback_msg*) {
    ++postAttempts;
    return nextPostResult;
}
""",
}

MAIN = r"""
#include "tasks.cpp"
#include <cassert>

int main()
{
#if defined(SMING_RELEASE) || defined(DISABLE_NETWORK)
    sming_dispatch_trace_t trace{};
    assert(!sming_dispatch_trace_take(&trace));
    assert(!sming_dispatch_stats_take(nullptr));
#else
    os_event_t storage[64]{};
    auto callback = +[](os_event_t* evt) {
        testClock += evt->par;
        if(evt->sig == 999) {
            nextPostResult = ERR_MEM;
            assert(system_os_post(USER_TASK_PRIO_1, 1000, 1));
            testClock += 100;
        }
    };
    assert(system_os_task(callback, USER_TASK_PRIO_1, storage, 64));
    start_sming_task_loop();
    sming_dispatch_trace_t trace{};
    assert(!sming_dispatch_trace_take(&trace));
    assert(!sming_dispatch_trace_take(nullptr));
    assert(diagnosticErrors == 1);
    auto dispatch = [] {
        assert(callbackMessage != nullptr);
        callbackMessage->fn(callbackMessage->arg);
    };
    // Ring rollover retains the newest 32 completions, across batches.
    testClock = 100;
    for(unsigned i = 1; i <= 40; ++i) {
        assert(system_os_post(USER_TASK_PRIO_1, i, i));
        dispatch();
    }
    const auto failedAt = uint64_t(testClock);
    nextPostResult = ERR_MEM;
    assert(system_os_post(USER_TASK_PRIO_1, 80, 5));
    testClock += 10;
    assert(system_os_post(USER_TASK_PRIO_1, 81, 6));
    assert(!eventQueueFlag); // Existing rejection recovery remains intact.
    // Completing queued work after failure must not overwrite the incident.
    dispatch();
    assert(sming_dispatch_trace_take(&trace));
    assert(trace.failed_us == failedAt && trace.failures == 2);
    assert(trace.queue_depth == 1 && !trace.active);
    assert(trace.current.sig == 0);
    assert(trace.count == 32 && trace.overwritten == 8);
    for(unsigned i = 0; i < trace.count; ++i) {
        assert(trace.entries[i].sig == i + 9);
        assert(trace.entries[i].par == i + 9);
        assert(trace.entries[i].elapsed_us == i + 9);
        assert(trace.entries[i].batch == i + 9);
        if(i) assert(trace.entries[i].started_us > trace.entries[i - 1].started_us);
    }
    assert(!sming_dispatch_trace_take(&trace)); // Exactly one report per incident.
    // Failure during a callback captures its elapsed-at-failure, not final duration.
    nextPostResult = ERR_OK;
    assert(system_os_post(USER_TASK_PRIO_1, 999, 25));
    const auto activeStart = uint64_t(testClock);
    dispatch();
    assert(eventQueue->entries.size() == 1); // Preserve initial-count batch limit.
    assert(sming_dispatch_trace_take(&trace));
    assert(trace.active && trace.count == 0 && trace.overwritten == 0);
    assert(trace.failures == 1 && trace.queue_depth == 1);
    assert(trace.current.sig == 999 && trace.current.par == 25);
    assert(trace.current.started_us == activeStart);
    assert(trace.current.elapsed_us == 25 && trace.failed_us == activeStart + 25);
    // Rearmed recorder works again, including 64-bit timestamps beyond 32-bit wrap.
    testClock = int64_t(UINT32_MAX) + 100;
    nextPostResult = ERR_OK;
    dispatch();
    nextPostResult = ERR_MEM;
    assert(system_os_post(USER_TASK_PRIO_1, 90, 1));
    assert(sming_dispatch_trace_take(&trace));
    assert(!trace.active && trace.count == 1);
    assert(trace.entries[0].sig == 1000 && trace.entries[0].elapsed_us == 1);
    assert(trace.entries[0].started_us == uint64_t(UINT32_MAX) + 100);
    sming_dispatch_stats_t stats{};
    assert(sming_dispatch_stats_take(&stats));
    assert(stats.mailbox_post_failures == 4);
    assert(stats.events == 44);
    assert(lockDepth == 0);
#endif
}
"""


class DispatchTraceTest(unittest.TestCase):
    def test_dispatcher(self):
        source = Path(os.environ.get("SMING_HOME", "/opt/sming/Sming"))
        source /= "Arch/Esp32/Components/esp32/src"
        self.assertTrue((source / "tasks.cpp").is_file())
        with tempfile.TemporaryDirectory(prefix="dispatch-trace-") as directory:
            root = Path(directory)
            for name, content in STUBS.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            (root / "main.cpp").write_text(MAIN)
            for defines in ([], ["-DSMING_RELEASE"], ["-DDISABLE_NETWORK"]):
                with self.subTest(defines=defines):
                    binary = root / "test"
                    subprocess.run(
                        ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                         *defines, "-I", str(root),
                         "-I", str(source), str(root / "main.cpp"), "-o", str(binary)],
                        check=True,
                    )
                    subprocess.run([str(binary)], check=True)

    def test_reporter(self):
        firmware = Path(__file__).resolve().parents[1]
        sming = Path(os.environ.get("SMING_HOME", "/opt/sming/Sming"))
        with tempfile.TemporaryDirectory(prefix="dispatch-report-") as directory:
            root = Path(directory)
            (root / "c_types.h").write_text(STUBS["c_types.h"])
            for defines in ([], ["-DARCH_ESP32"]):
                with self.subTest(defines=defines):
                    binary = root / "reporter"
                    subprocess.run(
                        ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DEVENT_MONITOR=1",
                         *defines, "-I", str(root),
                         "-I", str(firmware / "tests/callback_timing_stubs"),
                         "-I", str(firmware / "include"),
                         "-I", str(sming / "Arch/Esp32/Components/esp32/src/include"),
                         str(firmware / "tests/callback_timing_test.cpp"),
                         str(firmware / "app/CallbackTiming.cpp"), "-o", str(binary)],
                        check=True,
                    )
                    subprocess.run([str(binary)], check=True)

    def test_disabled_monitor_and_release_have_no_reporter_symbols(self):
        firmware = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="disabled-monitor-") as directory:
            root = Path(directory)
            source = root / "main.cpp"
            source.write_text("""
#include <CallbackTiming.h>
int main() {
    CallbackTiming::start();
    CallbackTiming::Scope scope(CallbackTiming::Id::LedUpdate);
}
""")
            for defines in ([], ["-DEVENT_MONITOR=0"],
                            ["-DEVENT_MONITOR=1", "-DSMING_RELEASE"]):
                with self.subTest(defines=defines):
                    binary = root / "disabled"
                    subprocess.run(
                        ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                         *defines, "-I", str(firmware / "include"), str(source),
                         str(firmware / "app/CallbackTiming.cpp"), "-o", str(binary)],
                        check=True,
                    )
                    subprocess.run([str(binary)], check=True)
                    symbols = subprocess.run(["nm", "-C", str(binary)], check=True,
                                             capture_output=True, text=True).stdout
                    self.assertNotIn("CallbackTiming::", symbols)


if __name__ == "__main__":
    unittest.main()
