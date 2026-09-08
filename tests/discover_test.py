"""Read-only discovery fixtures; no sockets or speakers are contacted."""
import contextlib
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import discover


class DiscoveryTests(unittest.TestCase):
    def test_discovery_failure_is_an_error(self):
        for result in [set(), OSError('network unavailable')]:
            with patch.object(discover, 'discover_addresses', side_effect=result if isinstance(result, Exception) else None,
                              return_value=result), patch.object(discover, 'probe') as probe, \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(discover.main([]), 1)
                probe.assert_not_called()

    def test_unreadable_discovered_players_fail(self):
        with patch.object(discover, 'discover_addresses', return_value={'192.0.2.1'}), \
                patch.object(discover, 'probe', side_effect=OSError('unreachable')), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(discover.main([]), 1)

    def test_only_discovered_addresses_are_probed(self):
        with patch.object(discover, 'discover_addresses', return_value={'192.0.2.1'}), \
                patch.object(discover, 'probe', return_value={'ip': '192.0.2.1'}) as probe, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(discover.main([]), 0)
            probe.assert_called_once_with('192.0.2.1')


if __name__ == '__main__':
    unittest.main()
