import contextlib
import io
import json
import pathlib
import tempfile
import unittest
from unittest.mock import Mock, patch

from log_statistics import BootSelection, CallbackTiming, DispatcherTiming, LogRecord, LogService, controller_ip, main, parse_args, write_plot


def record(log_id, sequence, *, name="led_update", calls=2, max_us=20, total_us=30, boot=1):
    return {
        "id": log_id,
        "boot": boot,
        "message": (
            f"CallbackTiming seq={sequence} name={name} calls={calls} "
            f"avg_us={total_us / calls:.1f} max_us={max_us} total_us={total_us}"
        ),
    }


class CliTests(unittest.TestCase):
    def test_collector_receive_timestamp_is_parsed_and_validated(self):
        value = record(1, 1)
        value["receivedAt"] = "2026-11-11T15:00:00.500Z"
        self.assertEqual(LogRecord.from_json(value).received_at, 1794409200.5)
        for invalid in ["bad", "2026-11-11T15:00:00", 123]:
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, "Invalid receivedAt"):
                LogRecord.from_json({**value, "receivedAt": invalid})

    def test_repeated_hosts_preserve_order(self):
        args = parse_args(["--host", "192.0.2.2", "--host", "192.0.2.3", "--log", "collector"])
        self.assertEqual(args.host, ["192.0.2.2", "192.0.2.3"])

    def test_multiple_hosts_have_independent_boots_statistics_and_plot_series(self):
        base = ["--host", "192.0.2.2", "--host", "192.0.2.3", "--log", "collector",
                "--callback-timing", "-b", "0", "--timeline", "calls/s"]
        boots = [BootSelection(2, 1, 5), BootSelection(7, 6, 10)]
        with patch("log_statistics.LogService.select_boot", side_effect=boots) as select, \
                patch("log_statistics.LogService.logs", side_effect=[
                    iter([LogRecord.from_json(record(1, 1, name="first", boot=2))]),
                    iter([LogRecord.from_json(record(6, 1, name="second", boot=7))]),
                ]) as logs, patch("log_statistics.write_plot") as plot, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(main(base), 0)
        self.assertEqual([call.args for call in select.call_args_list],
                         [("192.0.2.2", 0), ("192.0.2.3", 0)])
        self.assertEqual([call.args for call in logs.call_args_list], [
            ("192.0.2.2", ("CallbackTiming",), boots[0]),
            ("192.0.2.3", ("CallbackTiming",), boots[1]),
        ])
        modules = plot.call_args.args[1]["modules"]
        self.assertEqual([module["controller"] for module in modules],
                         ["192.0.2.2 (192.0.2.2)", "192.0.2.3 (192.0.2.3)"])
        self.assertEqual(list(modules[0]["parents"]), ["first"])
        self.assertEqual(list(modules[1]["parents"]), ["second"])
        self.assertLess(output.getvalue().index("Controller: 192.0.2.2"),
                        output.getvalue().index("Controller: 192.0.2.3"))

    def test_later_host_error_does_not_write_partial_comparison(self):
        with patch("log_statistics.LogService.logs", side_effect=[iter([]), OSError("collector unavailable")]), \
                patch("log_statistics.write_plot") as plot, \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(main(["--host", "192.0.2.2", "--host", "192.0.2.3",
                                   "--log", "collector", "--timeline"]), 2)
        plot.assert_not_called()
        self.assertIn("collector unavailable", errors.getvalue())

    def test_module_flag_and_relative_boot_offsets(self):
        base = ["--host", "192.0.2.2", "--log", "collector"]
        defaults = parse_args(base)
        self.assertFalse(defaults.callback_timing)
        self.assertIsNone(defaults.boot)
        self.assertEqual(defaults.sort, "average")
        self.assertIsNone(defaults.timeline)
        for selector in ["-b", "--boot"]:
            for offset in [0, -1, -2]:
                with self.subTest(selector=selector, offset=offset):
                    args = parse_args([*base, "--callback-timing", selector, str(offset)])
                    self.assertTrue(args.callback_timing)
                    self.assertEqual(args.boot, offset)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parse_args([*base, "-b", "1"])

    def test_sort_columns_and_invalid_column(self):
        base = ["--host", "192.0.2.2", "--log", "collector"]
        for column in ["function", "calls", "average", "max", "total", "calls/s", "ms/s", "included-in", "tree"]:
            with self.subTest(column=column):
                self.assertEqual(parse_args([*base, "--sort", column]).sort, column)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parse_args([*base, "--sort", "unknown"])

    def test_timeline_metrics_and_default(self):
        base = ["--host", "192.0.2.2", "--log", "collector"]
        self.assertEqual(parse_args([*base, "--timeline"]).timeline, "load")
        for metric in ["load", "total", "calls", "calls/s", "ms/s", "all"]:
            with self.subTest(metric=metric):
                self.assertEqual(parse_args([*base, "--timeline", metric]).timeline, metric)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parse_args([*base, "--timeline", "unknown"])

    def test_addresses_and_default_port(self):
        for address, expected in [
            ("collector.local", "http://collector.local:4821"),
            ("192.0.2.1:9000", "http://192.0.2.1:9000"),
            ("https://collector.local", "https://collector.local:4821"),
            ("::1", "http://[::1]:4821"),
            ("[::1]:9000", "http://[::1]:9000"),
        ]:
            with self.subTest(address=address):
                args = parse_args(["--host", "192.0.2.2", "--log", address])
                self.assertEqual(args.log, expected)

    def test_invalid_log_address(self):
        for address in ["ftp://collector", "collector:invalid", "collector:0", "collector/path", ""]:
            with self.subTest(address=address), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as err:
                    parse_args(["--host", "controller", "--log", address])
                self.assertEqual(err.exception.code, 2)

    def test_host_resolution(self):
        self.assertEqual(controller_ip("192.0.2.2"), "192.0.2.2")
        with patch("log_statistics.socket.getaddrinfo", return_value=[
            (2, 2, 17, "", ("192.0.2.2", 0)),
        ]):
            self.assertEqual(controller_ip("controller.local"), "192.0.2.2")

    def test_ambiguous_hostname_requires_explicit_ip(self):
        with patch("log_statistics.socket.getaddrinfo", return_value=[
            (2, 2, 17, "", ("192.0.2.2", 0)),
            (2, 2, 17, "", ("192.0.2.3", 0)),
        ]):
            with self.assertRaisesRegex(ValueError, "multiple addresses"):
                controller_ip("controller.local")


class DispatcherTimingTests(unittest.TestCase):
    @staticmethod
    def record(log_id=1, boot="boot:1", **changes):
        fields = {
            "seq": 1, "batches": 2, "events": 3, "avg_us": 1500.0,
            "max_us": 2000, "total_us": 3000, "max_events": 2,
            "queue_peak_sampled": 3, "event_full": 0, "mbox_attempts": 4, "mbox_full": 1,
        }
        fields.update(changes)
        return LogRecord(log_id, "DispatcherTiming " + " ".join(
            f"{key}={value}" for key, value in fields.items()
        ), boot)

    def test_weighted_durations_sums_peaks_and_rejection_windows(self):
        statistic = DispatcherTiming()
        first = self.record()
        statistic.consume(first)
        statistic.consume(first)
        statistic.consume(self.record(
            2, seq=3, batches=1, events=1, avg_us=6000, max_us=6000,
            total_us=6000, max_events=1, queue_peak_sampled=2,
            event_full=2, mbox_attempts=1, mbox_full=0,
        ))
        report = statistic.report()
        for expected in [
            "Observed blocks: 2; boots: 1; missing sequence blocks: 1",
            "Dispatcher batches: 3; processed events: 4",
            "average 3.000; max 6.000; total 9.000",
            "Maximum events per batch: 2",
            "Event queue sampled peak depth: 3",
            "Event queue rejected posts (event_full): 2",
            "Mailbox post attempts (mbox_attempts): 5",
            "Mailbox rejected posts (mbox_full / ERR_MEM): 1",
            "boot:1 seq=1 event_full=0 mbox_full=1 queue_peak_sampled=3",
            "boot:1 seq=3 event_full=2 mbox_full=0 queue_peak_sampled=2",
        ]:
            self.assertIn(expected, report)
        self.assertIsNone(statistic.plot_data())

    def test_sequence_restarts_are_separate_boots_and_zero_batches_are_valid(self):
        statistic = DispatcherTiming()
        statistic.consume(self.record())
        statistic.consume(self.record(2, boot="boot:2"))
        self.assertIn("Observed blocks: 2; boots: 2; missing sequence blocks: 0", statistic.report())
        empty = DispatcherTiming()
        empty.consume(self.record(
            batches=0, events=0, avg_us=0, max_us=0, total_us=0, max_events=0,
            queue_peak_sampled=0, mbox_attempts=0, mbox_full=0,
        ))
        report = empty.report()
        self.assertIn("average 0.000; max 0.000; total 0.000", report)
        self.assertNotIn("Rejection windows", report)

    def test_empty_and_unrelated_records(self):
        statistic = DispatcherTiming()
        statistic.consume(LogRecord.from_json(record(1, 1)))
        self.assertIn("No matching logs found.", statistic.report())

    def test_invalid_records_are_reported(self):
        for changes in [
            {"seq": -1}, {"batches": -1}, {"mbox_full": 5},
            {"avg_us": "nan"}, {"avg_us": "inf"}, {"max_us": 4000},
            {"total_us": 5000}, {"max_events": 4}, {"event_full": "oops"},
        ]:
            with self.subTest(changes=changes), self.assertRaisesRegex(ValueError, "Malformed DispatcherTiming"):
                DispatcherTiming().consume(self.record(**changes))
        for message in [
            "DispatcherTiming seq=1",
            self.record().message + " seq=2",
            self.record().message + " broken",
        ]:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, "Malformed DispatcherTiming"):
                DispatcherTiming().consume(LogRecord(1, message, "boot:1"))

    def test_conflicting_duplicates_fail(self):
        statistic = DispatcherTiming()
        statistic.consume(self.record())
        with self.assertRaisesRegex(ValueError, "Conflicting DispatcherTiming"):
            statistic.consume(self.record(2, mbox_full=0))

    def test_module_selection_and_default_collection(self):
        base = ["--host", "192.0.2.2", "--log", "collector"]
        for flags, terms in [
            ([], ("CallbackTiming", "DispatcherTiming")),
            (["--dispatcher-timing"], ("DispatcherTiming",)),
            (["--callback-timing", "--dispatcher-timing"], ("CallbackTiming", "DispatcherTiming")),
        ]:
            with self.subTest(flags=flags), \
                    patch("log_statistics.LogService.logs", return_value=iter([self.record()])) as logs, \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(main([*base, *flags]), 0)
                self.assertEqual(logs.call_args.args[1], terms)
                self.assertIn("Mailbox rejected posts", output.getvalue())

    def test_text_only_module_excluded_from_plots_and_alone_fails_explicitly(self):
        base = ["--host", "192.0.2.2", "--log", "collector", "--timeline"]
        with patch("log_statistics.LogService.logs", return_value=iter([])), \
                patch("log_statistics.write_plot") as plot, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(main(base), 0)
        self.assertEqual([module["name"] for module in plot.call_args.args[1]["modules"]], ["CallbackTiming"])
        with patch("log_statistics.LogService.logs", return_value=iter([])), \
                patch("log_statistics.write_plot") as plot, \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(main([*base, "--dispatcher-timing"]), 2)
        plot.assert_not_called()
        self.assertIn("include --callback-timing", errors.getvalue())


class CallbackTimingTests(unittest.TestCase):
    def test_header_format_applies_to_roots_before_or_after_header(self):
        for header_first in [True, False]:
            with self.subTest(header_first=header_first):
                statistic = CallbackTiming()
                header = LogRecord(1, "CallbackTiming seq=1 window_ms=5000 format=2", "boot:1")
                root = LogRecord.from_json(record(2, 1, name="ws_pump"))
                for item in ([header, root] if header_first else [root, header]):
                    statistic.consume(item)
                self.assertEqual(statistic.parent("ws_pump"), "-")
                self.assertEqual(statistic.plot_data()["parents"]["ws_pump"], "-")

    def test_tree_sort_indents_paths_without_merging_same_leaf_names(self):
        statistic = CallbackTiming()
        names = [
            "runtime_info/runtime_json", "led_update/light_state/render",
            "runtime_info", "led_update", "led_update/current_state/render",
            "led_update/light_state", "led_update/current_state",
        ]
        for log_id, name in enumerate(names, start=1):
            item = record(log_id, 1, name=name)
            item["message"] += " format=2"
            statistic.consume(LogRecord.from_json(item))
        rows = statistic.report("tree").splitlines()[-len(names):]
        expected = [
            "led_update", "  current_state", "    render",
            "  light_state", "    render", "runtime_info", "  runtime_json",
        ]
        for row, label in zip(rows, expected):
            self.assertTrue(row.startswith(label + " "), row)
            self.assertEqual(row.split()[1:5], ["2", "0.015", "0.020", "0.030"])
        self.assertTrue(rows[2].endswith("led_update/current_state"))
        self.assertTrue(rows[4].endswith("led_update/light_state"))
        self.assertEqual(len(statistic.plot_data()["boots"][0]["blocks"][0]["functions"]), len(names))

    def test_runtime_paths_determine_parents_without_merging_callers(self):
        statistic = CallbackTiming()
        names = [
            "led_update", "led_update/light_state", "led_update/light_state/root",
            "led_update/light_state/populate", "led_update/light_state/render",
            "led_update/light_state/normalize", "led_update/light_state/send",
            "led_update/light_state/send/ws_pump",
            "current_state", "current_state/send/ws_pump", "ws_pump", "runtime_collect",
        ]
        for log_id, name in enumerate(names, start=1):
            value = record(log_id, 1, name=name)
            value["message"] += " format=2"
            statistic.consume(LogRecord.from_json(value))
        self.assertEqual(statistic.parent("led_update/light_state/send/ws_pump"),
                         "led_update/light_state/send")
        self.assertEqual(statistic.parent("current_state/send/ws_pump"), "current_state/send")
        for name in ["current_state", "ws_pump", "runtime_collect"]:
            self.assertEqual(statistic.parent(name), "-")
        report = statistic.report("included-in")
        rows = report.splitlines()[-len(names):]
        self.assertEqual(len(rows), len(names))
        data = statistic.plot_data()
        self.assertEqual(data["parents"]["current_state/send/ws_pump"], "current_state/send")
        self.assertEqual(len(data["boots"][0]["blocks"][0]["functions"]), len(names))

    def test_paths_work_without_header_and_legacy_names_keep_hints(self):
        statistic = CallbackTiming()
        for log_id, name in enumerate(["led_update/light_state/render", "light_state"], start=1):
            statistic.consume(LogRecord.from_json(record(log_id, 1, name=name)))
        self.assertEqual(statistic.parent("led_update/light_state/render"), "led_update/light_state")
        self.assertEqual(statistic.parent("light_state"), "led_update")

    def test_drop_count_warns_and_invalid_versions_or_paths_fail(self):
        statistic = CallbackTiming()
        statistic.consume(LogRecord(1, "CallbackTiming seq=1 window_ms=5000 format=2 dropped_scopes=3", "boot:1"))
        self.assertIn("Firmware dropped 3 scopes", statistic.report())
        for suffix in ["format=3", "format=2 dropped_scopes=-1"]:
            with self.subTest(suffix=suffix), self.assertRaisesRegex(ValueError, "Malformed"):
                CallbackTiming().consume(LogRecord(1, "CallbackTiming seq=1 window_ms=5000 " + suffix, "boot:1"))
        for name in ["/led_update", "led_update/", "led_update//send", "led_update/<script>"]:
            value = record(1, 1, name=name)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Malformed"):
                CallbackTiming().consume(LogRecord.from_json(value))

    def test_timeline_metrics_use_actual_window_and_preserve_boots_and_gaps(self):
        statistic = CallbackTiming()
        for value in [
            {"id": 1, "boot": 1, "message": "CallbackTiming seq=10 window_ms=10000"},
            record(2, 10, calls=2, max_us=1000000, total_us=1000000),
            record(3, 12, calls=4, max_us=1000000, total_us=2000000),
            record(4, 1, boot=2, calls=1, max_us=500000, total_us=500000),
        ]:
            statistic.consume(LogRecord.from_json(value))
        data = statistic.plot_data()
        first, second = data["boots"]
        self.assertEqual(first["name"], "boot:1")
        self.assertEqual(first["blocks"][0]["functions"]["led_update"],
                         {"load": 10.0, "total": 1000.0, "calls": 2, "calls/s": 0.2, "ms/s": 100.0})
        block = first["blocks"][1]
        self.assertEqual(block["functions"]["led_update"],
                         {"load": 40.0, "total": 2000.0, "calls": 4, "calls/s": 0.8, "ms/s": 400.0})
        self.assertEqual((block["gap"], block["start"], block["end"]), (1, 15.0, 20.0))
        self.assertTrue(block["estimated"])
        self.assertEqual(second["name"], "boot:2")
        self.assertEqual(second["blocks"][0]["start"], 0)
        with self.assertRaisesRegex(ValueError, "Invalid timeline"):
            statistic.report(timeline="invalid")

    def test_timeline_marks_missing_function_and_zero_separately(self):
        statistic = CallbackTiming()
        statistic.consume(LogRecord.from_json(record(1, 1, max_us=0, total_us=0)))
        statistic.consume(LogRecord(2, "CallbackTiming seq=2 window_ms=5000", "boot:1"))
        statistic.consume(LogRecord.from_json(record(3, 3)))
        blocks = statistic.plot_data()["boots"][0]["blocks"]
        self.assertEqual(blocks[0]["functions"]["led_update"]["total"], 0)
        self.assertNotIn("led_update", blocks[1]["functions"])
        self.assertEqual(blocks[2]["functions"]["led_update"]["total"], 0.030)

    def test_timeline_wraps_and_large_gaps_are_compact(self):
        statistic = CallbackTiming()
        for log_id, sequence in enumerate([1, 2, 3, 4000000000], start=1):
            statistic.consume(LogRecord.from_json(record(log_id, sequence)))
        data = statistic.plot_data()
        blocks = data["boots"][0]["blocks"]
        self.assertEqual(len(blocks), 4)
        self.assertEqual(blocks[-1]["gap"], 3999999996)
        self.assertEqual(blocks[-1]["start"], 19999999995)
        self.assertLess(len(json.dumps(data)), 2000)

    def test_timeline_header_only_and_empty_data(self):
        statistic = CallbackTiming()
        self.assertIn("No matching logs", statistic.report(timeline="all"))
        self.assertEqual(statistic.plot_data()["boots"], [])
        statistic.consume(LogRecord(1, "CallbackTiming seq=1 window_ms=5000", "boot:1"))
        self.assertEqual(statistic.plot_data()["boots"][0]["blocks"][0]["functions"], {})

    def test_report_documents_inclusive_hierarchy_without_subtracting_children(self):
        statistic = CallbackTiming()
        for value in [
            record(1, 1, name="led_update", calls=1, max_us=2000, total_us=2000),
            record(2, 1, name="light_show", calls=1, max_us=1000, total_us=1000),
        ]:
            statistic.consume(LogRecord.from_json(value))
        report = statistic.report(timeline="load")
        self.assertIn("state_publish -> one deferred light_state, current_state or MQTT message", report)
        self.assertIn("runtime_info -> runtime_collect, runtime_json, runtime_enqueue", report)
        self.assertIn("ota_verify -> ota_hash", report)
        self.assertIn("Deferred callbacks belong to later batches", report)
        self.assertIn("TCP/IP MQTT reception need not belong to a batch", report)
        self.assertIn("do not sum parents and children or infer exclusive time", report)
        functions = statistic.plot_data()["boots"][0]["blocks"][0]["functions"]
        self.assertEqual(functions["led_update"]["load"], 0.040)
        self.assertEqual(functions["light_show"]["load"], 0.020)
        self.assertEqual(statistic.plot_data()["parents"]["light_show"], "led_update")

    def test_plot_is_self_contained_and_embedded_data_is_escaped(self):
        data = {"controller": "</script><script>alert(1)</script>", "metric": "all", "modules": []}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "plot.html"
            write_plot(path, data)
            html = path.read_text()
        self.assertNotIn(data["controller"], html)
        self.assertNotIn("__PLOT_DATA__", html)
        self.assertNotIn("<script src=", html)
        encoded = html.split('<script id="data" type="application/json">')[1].split("</script>")[0]
        self.assertEqual(json.loads(encoded), data)

    def test_plot_aligns_controllers_to_shared_receive_origin_not_device_windows(self):
        modules = []
        for host, stamps in [("first", [100, 105, 120]), ("second", [108, 113])]:
            statistic = CallbackTiming()
            for index, stamp in enumerate(stamps):
                seq = index + 1 if index < 2 else 5
                statistic.consume(LogRecord(index + 1,
                    f"CallbackTiming seq={seq} window_ms=4290000000", "boot:1",
                    received_at=stamp))
                statistic.consume(LogRecord.from_json(record(index + 10, seq)))
            modules.append({**statistic.plot_data(), "controller": host})
        data = {"controller": "two", "metric": "calls/s", "modules": modules}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "plot.html"
            write_plot(path, data)
            encoded = path.read_text().split('<script id="data" type="application/json">')[1].split("</script>")[0]
            saved = json.loads(encoded)
        first, second = [module["boots"][0]["blocks"] for module in saved["modules"]]
        self.assertEqual([(b["start"], b["end"]) for b in first], [(0, 0), (0, 5), (15, 20)])
        self.assertEqual([(b["start"], b["end"]) for b in second], [(3, 8), (8, 13)])
        self.assertEqual(first[2]["gap_start"], 5)
        self.assertEqual(saved["modules"][0]["boots"][0]["duration"], 20)
        self.assertIn("time_origin", saved)

    def test_plot_without_receive_timestamps_fails_explicitly(self):
        statistic = CallbackTiming()
        statistic.consume(LogRecord.from_json(record(1, 1)))
        with tempfile.TemporaryDirectory() as directory, \
                self.assertRaisesRegex(ValueError, "requires collector receivedAt"):
            write_plot(pathlib.Path(directory) / "plot.html",
                       {"modules": [statistic.plot_data()]})

    def test_weighted_average_max_total_and_sequence_gaps(self):
        statistic = CallbackTiming()
        for value in [
            {"id": 1, "boot": 1, "message": "CallbackTiming seq=10 window_ms=5100 inclusive=1 order=avg_us"},
            record(2, 10),
            record(3, 12, calls=8, max_us=10, total_us=40),
            record(4, 10, boot=2, calls=1, max_us=40, total_us=40),
        ]:
            statistic.consume(LogRecord.from_json(value))
        report = statistic.report()
        self.assertIn("Observed blocks: 3; boots: 2; missing sequence blocks: 1", report)
        self.assertIn("15.100 s (2 missing headers estimated at 5 s)", report)
        row = report.splitlines()[-1].split()
        self.assertEqual(row, ["led_update", "11", "0.010", "0.040", "0.110", "0.728", "0.007", "-"])
        self.assertIn("Average ms", report)
        self.assertIn("Max ms", report)
        self.assertIn("Total ms", report)

    def test_duplicates_and_reordered_headers_are_not_double_counted(self):
        statistic = CallbackTiming()
        for value in [
            record(1, 1),
            record(2, 1),
            {"id": 3, "boot": 1, "message": "CallbackTiming seq=1 window_ms=5200 inclusive=1 order=avg_us"},
        ]:
            statistic.consume(LogRecord.from_json(value))
        self.assertEqual(statistic.report().splitlines()[-1].split(),
                         ["led_update", "2", "0.015", "0.020", "0.030", "0.385", "0.006", "-"])
        self.assertIn("5.200 s", statistic.report())

    def test_millisecond_conversion_and_each_sort_column(self):
        statistic = CallbackTiming()
        for value in [
            record(1, 1, name="alpha", calls=10, max_us=2000, total_us=10000),
            record(2, 1, name="bravo", calls=2, max_us=6000, total_us=8000),
            record(3, 1, name="charlie", calls=3, max_us=9000, total_us=9000),
        ]:
            statistic.consume(LogRecord.from_json(value))
        for column, expected in [
            ("function", ["alpha", "bravo", "charlie"]),
            ("calls", ["alpha", "charlie", "bravo"]),
            ("average", ["bravo", "charlie", "alpha"]),
            ("max", ["charlie", "bravo", "alpha"]),
            ("total", ["alpha", "charlie", "bravo"]),
            ("calls/s", ["alpha", "charlie", "bravo"]),
            ("ms/s", ["alpha", "charlie", "bravo"]),
        ]:
            with self.subTest(column=column):
                rows = [line.split() for line in statistic.report(column).splitlines()[-3:]]
                self.assertEqual([row[0] for row in rows], expected)
        self.assertEqual(statistic.report("function").splitlines()[-3].split(),
                         ["alpha", "10", "1.000", "2.000", "10.000", "2.000", "2.000", "-"])
        with self.assertRaisesRegex(ValueError, "Invalid sort"):
            statistic.report("unknown")

    def test_sort_ties_use_function_name(self):
        statistic = CallbackTiming()
        for log_id, name in [(1, "bravo"), (2, "alpha")]:
            statistic.consume(LogRecord.from_json(record(log_id, 1, name=name)))
        for column in ["calls", "average", "max", "total", "calls/s", "ms/s"]:
            with self.subTest(column=column):
                rows = statistic.report(column).splitlines()[-2:]
                self.assertEqual([row.split()[0] for row in rows], ["alpha", "bravo"])

    def test_rates_use_all_observed_time_across_boots_and_inactive_windows(self):
        statistic = CallbackTiming()
        for value in [
            record(1, 1, name="led_update/light_state", calls=20, max_us=200000, total_us=2000000),
            {"id": 2, "boot": 1, "message": "CallbackTiming seq=1 window_ms=2000 format=2"},
            {"id": 3, "boot": 1, "message": "CallbackTiming seq=3 window_ms=3000 format=2"},
            record(4, 1, boot=2, name="led_update/light_state", calls=10,
                   max_us=200000, total_us=1000000),
        ]:
            statistic.consume(LogRecord.from_json(value))
        # 2 + 3 + estimated 5 seconds; the missing seq=2 contributes no time.
        row = statistic.report().splitlines()[-1].split()
        self.assertEqual(row, ["led_update/light_state", "30", "100.000", "200.000",
                               "3000.000", "3.000", "300.000", "led_update"])
        data = statistic.plot_data()
        first = data["boots"][0]["blocks"][0]["functions"]["led_update/light_state"]
        self.assertEqual(first["calls/s"], 10)
        self.assertEqual(first["ms/s"], 1000)
        self.assertEqual(first["ms/s"], first["load"] * 10)
        self.assertNotIn("led_update/light_state", data["boots"][0]["blocks"][1]["functions"])

    def test_long_reporting_window_warns_without_silently_changing_duration(self):
        statistic = CallbackTiming()
        statistic.consume(LogRecord(1, "CallbackTiming seq=1 window_ms=4290000000", "boot:1"))
        statistic.consume(LogRecord.from_json(record(2, 1)))
        report = statistic.report()
        self.assertIn("WARNING: Reporting windows exceed 60 s", report)
        self.assertIn("boot:1 seq=1", report)
        self.assertIn("Observed window time: 4290000.000 s", report)
        self.assertEqual(statistic.plot_data()["boots"][0]["duration"], 4290000)

    def test_table_shows_parent_for_every_nested_measurement_and_preserves_sort(self):
        names = [
            "led_update", "light_show", "light_state", "current_state",
            "runtime_info", "runtime_collect", "runtime_json", "runtime_enqueue",
            "ota_verify", "ota_hash", "ws_pump", "mqtt_receive", "future_bucket",
        ]
        expected_parents = {
            "light_show": "led_update",
            "light_state": "led_update",
            "current_state": "led_update",
            "runtime_collect": "runtime_info",
            "runtime_json": "runtime_info",
            "runtime_enqueue": "runtime_info",
            "ota_hash": "ota_verify",
            "ws_pump": "synchronous caller",
        }
        statistic = CallbackTiming()
        for log_id, name in enumerate(names, start=1):
            statistic.consume(LogRecord.from_json(record(log_id, 1, name=name)))
        for column in ["function", "average", "included-in"]:
            with self.subTest(column=column):
                report = statistic.report(column)
                self.assertIn("Included in", report)
                rows = [line.split() for line in report.splitlines()[-len(names):]]
                expected_order = sorted(
                    names,
                    key=lambda name: (expected_parents.get(name, "-"), name)
                    if column == "included-in" else ("", name),
                )
                self.assertEqual([row[0] for row in rows], expected_order)
                for row in rows:
                    self.assertEqual(row[1:5], ["2", "0.015", "0.020", "0.030"])
                    self.assertEqual(row[5:7], ["0.400", "0.006"])
                    self.assertEqual(" ".join(row[7:]), expected_parents.get(row[0], "-"))

    def test_header_only_and_no_matches(self):
        statistic = CallbackTiming()
        statistic.consume(LogRecord(1, "DispatcherTiming seq=1", "boot:1"))
        self.assertIn("No matching logs", statistic.report())
        statistic.consume(LogRecord(2, "CallbackTiming seq=1 window_ms=5000", "boot:1"))
        self.assertIn("Observed blocks: 1", statistic.report())
        self.assertIn("Function", statistic.report())

    def test_nonce_distinguishes_boots_and_missing_boot_warns(self):
        statistic = CallbackTiming()
        for log_id, nonce in [(1, "a"), (2, "b")]:
            value = record(log_id, 1)
            value["bootNonce"] = nonce
            statistic.consume(LogRecord.from_json(value))
        statistic.consume(LogRecord(3, record(3, 1)["message"], "unknown"))
        self.assertIn("Observed blocks: 3", statistic.report())
        self.assertIn("WARNING", statistic.report())

    def test_invalid_and_conflicting_records_fail_explicitly(self):
        for message in [
            "CallbackTiming seq=1 name=led_update calls=oops",
            "CallbackTiming seq=1 window_ms=0",
            "CallbackTiming seq=-1 window_ms=5000",
            "CallbackTiming seq=1 seq=2 window_ms=5000",
            "CallbackTiming seq=1 name=x calls=1 avg_us=nan max_us=10 total_us=10",
        ]:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, "log 1"):
                CallbackTiming().consume(LogRecord(1, message, "boot:1"))
        statistic = CallbackTiming()
        statistic.consume(LogRecord.from_json(record(1, 1)))
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            statistic.consume(LogRecord.from_json(record(2, 1, total_us=31)))


class LogServiceTests(unittest.TestCase):
    def test_boot_selection_uses_boot_order_not_timing_matches(self):
        service = LogService("http://collector:4821")
        service.get = Mock(return_value={"items": [
            {"boot": 5, "firstId": 10, "lastId": 20},
            {"boot": 9, "firstId": 40, "lastId": 50},
            {"boot": 7, "firstId": 25, "lastId": 35},
        ]})
        for offset, number in [(0, 9), (-1, 7), (-2, 5)]:
            with self.subTest(offset=offset):
                self.assertEqual(service.select_boot("192.0.2.2", offset).number, number)
        service.get.assert_called_with("/api/v1/boots", ip="192.0.2.2")
        with self.assertRaisesRegex(ValueError, "unavailable"):
            service.select_boot("192.0.2.2", -3)

    def test_missing_and_invalid_boot_metadata_fail(self):
        for page in [
            {"items": []},
            {},
            {"items": [{"boot": 1, "firstId": 20, "lastId": 10}]},
            {"items": [{"boot": "1", "firstId": 10, "lastId": 20}]},
        ]:
            service = LogService("http://collector:4821")
            service.get = Mock(return_value=page)
            with self.subTest(page=page), self.assertRaises(ValueError):
                service.select_boot("192.0.2.2", 0)

    def test_search_excludes_other_boots_and_outside_snapshot(self):
        service = LogService("http://collector:4821")
        service.search = Mock(return_value=[
            LogRecord.from_json(record(10, 1, boot=2)),
            LogRecord.from_json({**record(20, 1, boot=3), "bootNonce": "abc"}),
            LogRecord.from_json(record(30, 1, boot=2)),
            LogRecord.from_json(record(40, 2, boot=3)),
        ])
        logs = list(service.logs("192.0.2.2", ("CallbackTiming",), BootSelection(3, 20, 35)))
        self.assertEqual([log.id for log in logs], [20])

    def test_pagination_is_bounded_to_selected_boot_and_filters_interleaved_boots(self):
        service = LogService("http://collector:4821")
        service.search = Mock(return_value=None)
        service.get = Mock(side_effect=[
            {"items": [record(20, 1, boot=3), record(25, 1, boot=2)], "nextAfter": 30},
            {"items": [record(30, 2, boot=3), record(40, 1, boot=4)], "nextAfter": None},
        ])
        logs = list(service.logs("192.0.2.2", ("CallbackTiming",), BootSelection(3, 20, 35)))
        self.assertEqual([log.id for log in logs], [20, 30])
        self.assertEqual(service.get.call_args_list[0].kwargs["from"], 20)
        self.assertEqual(service.get.call_count, 2)

    def test_module_selection_excludes_unselected_modules(self):
        class OtherStatistic:
            cli_option = "other"
            search_term = "Other"

            def __init__(self):
                raise AssertionError("unselected module instantiated")

        with patch("log_statistics.STATISTICS", (CallbackTiming, OtherStatistic)), \
                patch("log_statistics.LogService.logs", return_value=iter([])) as logs, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(main(["--host", "192.0.2.2", "--log", "collector", "--callback-timing"]), 0)
        self.assertEqual(logs.call_args.args, ("192.0.2.2", ("CallbackTiming",), None))

    def test_main_passes_selected_boot_and_reports_unavailable_boot(self):
        boot = BootSelection(9, 40, 50)
        with patch("log_statistics.LogService.select_boot", return_value=boot) as select, \
                patch("log_statistics.LogService.logs", return_value=iter([])) as logs, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(main(["--host", "192.0.2.2", "--log", "collector", "-b", "-1"]), 0)
        select.assert_called_once_with("192.0.2.2", -1)
        self.assertEqual(logs.call_args.args[-1], boot)
        self.assertIn("Selected boot: 9 (offset -1)", output.getvalue())
        with patch("log_statistics.LogService.select_boot", side_effect=ValueError("unavailable")), \
                patch("log_statistics.LogService.logs") as logs, \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(main(["--host", "192.0.2.2", "--log", "collector", "-b", "-99"]), 2)
        logs.assert_not_called()
        self.assertIn("unavailable", errors.getvalue())

    def test_main_passes_sort_column_to_module(self):
        with patch("log_statistics.LogService.logs", return_value=iter([])), \
                patch("log_statistics.CallbackTiming.report", return_value="test report") as report, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(main([
                "--host", "192.0.2.2", "--log", "collector", "--callback-timing", "--sort", "total",
            ]), 0)
        report.assert_called_once_with("total", None)

    def test_main_passes_timeline_metric_to_module(self):
        with patch("log_statistics.LogService.logs", return_value=iter([])), \
                patch("log_statistics.CallbackTiming.report", return_value="test graph") as report, \
                patch("log_statistics.write_plot") as plot, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(main([
                "--host", "192.0.2.2", "--log", "collector", "--timeline", "all",
            ]), 0)
        report.assert_called_once_with("average", "all")
        self.assertEqual(plot.call_args.args[0], pathlib.Path("log-statistics.html"))
        self.assertEqual(plot.call_args.args[1]["metric"], "all")

    def test_plot_path_enables_all_metrics_and_write_failure_is_reported(self):
        base = ["--host", "192.0.2.2", "--log", "collector", "--plot", "my-plot.html"]
        with patch("log_statistics.LogService.logs", return_value=iter([])), \
                patch("log_statistics.write_plot") as plot, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(main(base), 0)
        self.assertEqual(plot.call_args.args[0], pathlib.Path("my-plot.html"))
        self.assertEqual(plot.call_args.args[1]["metric"], "all")
        with patch("log_statistics.LogService.logs", return_value=iter([])), \
                patch("log_statistics.write_plot", side_effect=OSError("permission denied")), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(main(base), 2)
        self.assertIn("permission denied", errors.getvalue())

    def test_search_filters_ip_context_and_duplicate_matches(self):
        service = LogService("http://collector:4821")
        wanted = record(10, 1)
        service.get = Mock(return_value={"snippets": [
            {"ip": "192.0.2.2", "matchId": 10, "rows": [record(9, 1, name="other"), wanted]},
            {"ip": "192.0.2.3", "matchId": 11, "rows": [record(11, 1)]},
        ]})
        logs = list(service.logs("192.0.2.2", ("CallbackTiming", "led_update")))
        self.assertEqual([log.id for log in logs], [10])
        self.assertEqual(service.get.call_count, 2)

    def test_capped_search_falls_back_to_inclusive_paging_with_snapshot_end(self):
        service = LogService("http://collector:4821")
        service.get = Mock(side_effect=[
            {"snippets": [{}] * 200},
            {"items": [record(50, 3)]},
            {"items": [record(10, 1)], "nextAfter": 30},
            {"items": [record(30, 2), record(50, 3), record(60, 4)], "nextAfter": None},
        ])
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            logs = list(service.logs("192.0.2.2", ("CallbackTiming",)))
        self.assertEqual([log.id for log in logs], [10, 30, 50])
        self.assertIn("avoid truncated statistics", errors.getvalue())
        self.assertEqual(service.get.call_args_list[-1].kwargs["from"], 30)

    def test_empty_search_does_not_fetch_full_logs(self):
        service = LogService("http://collector:4821")
        service.get = Mock(return_value={"snippets": []})
        self.assertEqual(list(service.logs("192.0.2.2", ("CallbackTiming",))), [])
        service.get.assert_called_once()

    def test_invalid_cursor_and_records_fail(self):
        for page in [
            {"items": [record(10, 1)], "nextAfter": 10},
            {"items": [record(10, 1), record(9, 1)], "nextAfter": None},
            {"items": [], "nextAfter": 30},
            {"error": "bad request"},
        ]:
            service = LogService("http://collector:4821")
            service.search = Mock(return_value=None)
            service.get = Mock(side_effect=[{"items": [record(50, 1)]}, page])
            with self.subTest(page=page), self.assertRaises(ValueError):
                list(service.logs("192.0.2.2", ("CallbackTiming",)))

    def test_http_query_encoding(self):
        response = io.BytesIO(json.dumps({"snippets": []}).encode())
        with patch("log_statistics.urllib.request.urlopen", return_value=response) as request:
            LogService("http://collector:4821").get("/api/v1/search", q="CallbackTiming test")
        self.assertEqual(request.call_args.args[0],
                         "http://collector:4821/api/v1/search?q=CallbackTiming+test")

    def test_main_prints_statistics_and_surfaces_errors(self):
        with patch("log_statistics.LogService.logs", return_value=iter([
            LogRecord.from_json(record(1, 1)),
        ])), contextlib.redirect_stdout(io.StringIO()) as output:
            result = main(["--host", "192.0.2.2", "--log", "collector"])
        self.assertEqual(result, 0)
        self.assertIn("led_update", output.getvalue())
        with patch("log_statistics.LogService.logs", side_effect=OSError("unavailable")), \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(main(["--host", "192.0.2.2", "--log", "collector"]), 2)
        self.assertIn("unavailable", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
