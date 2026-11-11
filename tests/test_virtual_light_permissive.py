import contextlib
import io
import unittest
from unittest.mock import AsyncMock, Mock, patch

from virtual_light_exerciser import Exerciser, Light, parse_args


class PermissiveTests(unittest.IsolatedAsyncioTestCase):
    def exerciser(self, *flags):
        args = parse_args(["controller", "--permissive", "--dwell", "0", *flags])
        with patch("virtual_light_exerciser.ApiLog", return_value=Mock()):
            exerciser = Exerciser(args)
        exerciser.discover = AsyncMock(return_value=[
            Light(0, "one", "One", "SINGLE", True),
            Light(1, "two", "Two", "SINGLE", False),
        ])
        exerciser.http.post = AsyncMock(return_value={"success": True})
        exerciser.read_color = AsyncMock(return_value={"hsv": {"v": 0}})
        return exerciser

    def test_flag_is_opt_in(self):
        self.assertFalse(parse_args(["controller"]).permissive)
        self.assertTrue(parse_args(["controller", "--permissive"]).permissive)

    async def test_errors_and_rejections_continue_without_reboot_or_websocket(self):
        exerciser = self.exerciser("--skip-fades", "--no-restore")
        exerciser.http.post.side_effect = [
            OSError("offline"), {"success": False}, {"error": "busy"},
            {"success": True},
        ]
        with patch("virtual_light_exerciser.solid_plan", return_value=[
            ("first", "hsv", {"v": 1}), ("second", "hsv", {"v": 0}),
        ]), patch("virtual_light_exerciser.reboot_controller") as reboot, \
                patch.object(exerciser.ws, "connect") as connect, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(await exerciser.run(), 0)
        self.assertEqual(exerciser.http.post.await_count, 4)
        self.assertEqual(exerciser.http.post.call_args_list[-1].args[1]["light"], 1)
        reboot.assert_not_called()
        connect.assert_not_called()
        self.assertIn("3 errors reported", output.getvalue())

    async def test_snapshot_and_restore_errors_do_not_stop_later_lights(self):
        exerciser = self.exerciser("--skip-fades")
        exerciser.read_color.side_effect = [OSError("cannot read"), {"hsv": {"v": 0}}]
        exerciser.http.post.side_effect = [{"success": True}, {"success": True}, OSError("restore failed")]
        with patch("virtual_light_exerciser.solid_plan", return_value=[("off", "hsv", {"v": 0})]), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(await exerciser.run(), 0)
        self.assertEqual(exerciser.http.post.await_count, 3)
        self.assertIn("restore failed", output.getvalue())

    async def test_discovery_failure_remains_fatal(self):
        exerciser = self.exerciser()
        exerciser.discover.return_value = []
        with self.assertRaisesRegex(RuntimeError, "no matching"):
            await exerciser.run()

    async def test_soak_continues_after_command_error_and_turns_off_without_collector(self):
        exerciser = self.exerciser("--soak-hours", "1")
        exerciser.http.post.side_effect = [OSError("offline")] + [{"success": True}] * 5
        with patch("virtual_light_exerciser.soak_commands", side_effect=lambda light, _: [
            exerciser.command(light, "hsv", {"v": 0})
        ]), patch("virtual_light_exerciser.time.monotonic", side_effect=[0, 0, 0, 4000]), \
                patch("virtual_light_exerciser.asyncio.sleep", new_callable=AsyncMock), \
                patch.object(exerciser.log_http, "get") as collector, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(await exerciser.run(), 0)
        collector.assert_not_called()
        self.assertEqual(exerciser.http.post.await_count, 6)
        self.assertEqual(exerciser.http.post.call_args_list[-1].args[1]["raw"], [0])
