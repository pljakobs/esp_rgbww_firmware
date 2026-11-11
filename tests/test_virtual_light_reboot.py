import unittest
from unittest.mock import AsyncMock, Mock, patch

from virtual_light_exerciser import Http, parse_args, reboot_controller


class RebootTests(unittest.IsolatedAsyncioTestCase):
    async def test_waits_for_offline_then_online_and_preserves_auth(self):
        http = Http("http://controller", "test-password", 5, Mock())
        http.post = AsyncMock()
        probe = AsyncMock()
        probe.get.side_effect = [{"app": {}}, OSError("offline"), {"app": {}}]
        with patch("virtual_light_exerciser.Http", return_value=probe), \
                patch("virtual_light_exerciser.asyncio.sleep", new_callable=AsyncMock):
            await reboot_controller(http, 2)
        http.post.assert_awaited_once_with("/system", {"cmd": "restart"})
        self.assertEqual(probe.get.await_count, 3)
        self.assertEqual(probe.headers, http.headers)

    async def test_rejected_restart_does_not_poll(self):
        http = Http("http://controller", None, 5, Mock())
        http.post = AsyncMock(side_effect=RuntimeError("HTTP 400: UpdateConflict"))
        with patch("virtual_light_exerciser.Http") as probe:
            with self.assertRaisesRegex(RuntimeError, "UpdateConflict"):
                await reboot_controller(http, 1)
        probe.assert_not_called()

    async def test_accepted_restart_without_disconnect_fails(self):
        http = Http("http://controller", None, 5, Mock())
        http.post = AsyncMock()
        probe = AsyncMock()
        probe.get.return_value = {"app": {}}
        with patch("virtual_light_exerciser.Http", return_value=probe):
            with self.assertRaisesRegex(RuntimeError, "never went offline"):
                await reboot_controller(http, 0.01)

    async def test_offline_without_recovery_fails(self):
        http = Http("http://controller", None, 5, Mock())
        http.post = AsyncMock()
        probe = AsyncMock()
        probe.get.side_effect = OSError("offline")
        with patch("virtual_light_exerciser.Http", return_value=probe):
            with self.assertRaisesRegex(RuntimeError, "last connection error"):
                await reboot_controller(http, 0.01)

    async def test_auth_failure_during_poll_is_not_hidden(self):
        http = Http("http://controller", None, 5, Mock())
        http.post = AsyncMock()
        probe = AsyncMock()
        probe.get.side_effect = RuntimeError("HTTP 401")
        with patch("virtual_light_exerciser.Http", return_value=probe):
            with self.assertRaisesRegex(RuntimeError, "HTTP 401"):
                await reboot_controller(http, 1)

    def test_cli_defaults_and_opt_out(self):
        self.assertFalse(parse_args(["controller"]).no_reboot)
        self.assertTrue(parse_args(["controller", "--no-reboot"]).no_reboot)


if __name__ == "__main__":
    unittest.main()
