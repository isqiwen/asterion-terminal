"""Install a self-contained process double in a disposable test directory.

The native helper receives an empty environment, including in production.
"""
from pathlib import Path
import sys


def install(directory):
    directory = Path(directory)
    directory.mkdir()
    helper = directory / 'keychain'
    helper.write_text(f'#!{sys.executable}\n' + '''import hashlib
from pathlib import Path
import sys
root = Path(__file__).parent
command, account = sys.argv[1:]
entry = root / hashlib.sha256(account.encode()).hexdigest()
if command == 'get':
    if not entry.exists(): sys.exit(3)
    sys.stdout.write(entry.read_text())
elif command == 'set':
    entry.write_text(sys.stdin.read())
elif command == 'delete':
    entry.unlink(missing_ok=True)
else:
    sys.exit(2)
''')
    helper.chmod(0o700)
    return str(helper.resolve())
