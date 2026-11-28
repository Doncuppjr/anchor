import asyncio
import contextlib
import io
import sys
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from ble_dfu import dfu_connection


class ConnectionCleanupTest(unittest.IsolatedAsyncioTestCase):
    async def test_waits_for_normal_disconnect(self):
        client = AsyncMock()
        with contextlib.redirect_stderr(io.StringIO()) as log:
            async with dfu_connection(client) as connected:
                self.assertIs(connected, client)
                client.connect.assert_awaited_once()
        client.disconnect.assert_awaited_once()
        self.assertEqual(log.getvalue(), "")

    async def test_stalled_disconnect_cannot_hide_transfer_failure(self):
        client = AsyncMock()
        cancelled = asyncio.Event()

        async def stuck_disconnect():
            try:
                await asyncio.Event().wait()
            finally:
                cancelled.set()

        client.disconnect.side_effect = stuck_disconnect
        with patch("ble_dfu.DISCONNECT_TIMEOUT", 0.01):
            with contextlib.redirect_stderr(io.StringIO()) as log:
                with self.assertRaisesRegex(RuntimeError, "transfer failed"):
                    async with dfu_connection(client):
                        raise RuntimeError("transfer failed")
        self.assertTrue(cancelled.is_set())
        self.assertIn("Timed out closing", log.getvalue())


if __name__ == "__main__":
    unittest.main()
