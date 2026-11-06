"""Lab logs identify the caller rather than the logging helper."""
import inspect
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, sys.argv.pop(1))
from asterion_lab.log import ProcessLog


class LogTest(unittest.TestCase):
    def test_default_pattern_includes_call_site(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'config').mkdir()
            (root / 'config/logging.json').write_text(json.dumps({
                'console': {'enabled': False, 'level': 'info'},
                'file': {'enabled': True, 'level': 'info',
                         'path': '../logs/lab.log', 'retention_days': 30}}))
            log = ProcessLog('asterion_lab', root)
            line = inspect.currentframe().f_lineno + 1
            log.write('info', 'lab', 'started', count=2)
            text = next((root / 'logs').glob('*.log')).read_text()
            self.assertRegex(text, r'^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} [+-]\d{2}:\d{2}\]')
            self.assertIn(f'[info] [asterion_lab] [log_test.py:{line}] lab started count=2', text)

    def test_exception_log_keeps_the_traceback(self):
        from contextlib import redirect_stderr
        from io import StringIO
        with tempfile.TemporaryDirectory() as directory, redirect_stderr(StringIO()) as output:
            log = ProcessLog('asterion_lab', Path(directory))
            try:
                raise RuntimeError("fixture research failure")
            except RuntimeError:
                log.write('error', 'lab', 'service.failed')
            text = output.getvalue()
            self.assertIn('Traceback (most recent call last):', text)
            self.assertIn('raise RuntimeError("fixture research failure")', text)
            self.assertIn('RuntimeError: fixture research failure', text)


if __name__ == '__main__':
    unittest.main()
