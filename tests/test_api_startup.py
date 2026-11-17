"""Port conflicts must never send Studio users to an unrelated server."""

import errno
import unittest
from unittest.mock import patch

from api.server import ApiServer, Settings, bind_api_server


class ServerStartupTests(unittest.TestCase):
    def test_occupied_port_falls_back_only_when_enabled(self):
        settings = Settings(workers=1, queue_size=1)
        with ApiServer(('127.0.0.1', 0), None, settings) as occupied:
            port = occupied.server_port
            with self.assertRaises(OSError):
                bind_api_server('127.0.0.1', port, None, settings)
            with bind_api_server('127.0.0.1', port, None, settings, auto_port=True) as server:
                self.assertNotEqual(server.server_port, port)
                self.assertGreater(server.server_port, 0)
                # The original listener remains owned by its original server.
                self.assertEqual(occupied.socket.getsockname()[1], port)

    def test_unrelated_socket_errors_are_not_hidden(self):
        error = OSError(errno.EACCES, 'permission denied')
        with patch('api.server.ApiServer', side_effect=error) as constructor:
            with self.assertRaises(OSError) as raised:
                bind_api_server('127.0.0.1', 8080, None, Settings(), auto_port=True)
            self.assertIs(raised.exception, error)
            constructor.assert_called_once()


if __name__ == '__main__':
    unittest.main()
