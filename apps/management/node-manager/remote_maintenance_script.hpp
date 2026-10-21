#pragma once
#include <string_view>

namespace asterion::terminal {

// Uploaded and executed as the unprivileged node account. System service control
// remains in the root-owned, restricted asterion_host tool.
inline constexpr std::string_view remote_maintenance_script = R"PY(
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root, stage = Path(sys.argv[1]), Path(sys.argv[2])
action, operation, version, preserve, port, bind = sys.argv[3:]
preserve = preserve == 'true'
state = root / 'state'

def sync(folder):
    fd = os.open(folder, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)

def write(file, data, mode=0o600):
    fd, name = tempfile.mkstemp(prefix='.publish-', dir=file.parent)
    try:
        with os.fdopen(fd, 'wb') as output:
            os.fchmod(output.fileno(), mode)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(name, file)
        sync(file.parent)
    finally:
        if os.path.exists(name):
            os.unlink(name)

for folder in (root, state):
    if folder.is_symlink() or not folder.is_dir() or folder.stat().st_uid != os.getuid():
        raise ValueError('Invalid owned node directory: ' + str(folder))
record = root / 'maintenance-operation.json'
intent = dict(operation=operation, action=action, version=version, preserve=preserve)
lock = os.open(state / 'agent.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
try:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if record.exists():
        previous = json.loads(record.read_text())
        if previous.get('complete') and all(previous.get(k) == v for k, v in intent.items()):
            print(json.dumps(dict(complete=True)))
            sys.exit(0)
        if not previous.get('complete') and any(previous.get(k) != v for k, v in intent.items()):
            raise ValueError('Another node maintenance operation is pending')
    if not record.exists() or json.loads(record.read_text()).get('complete'):
        if json.loads((state / 'maintenance.json').read_text())['operation'] != operation:
            raise ValueError('Node maintenance ownership differs')
        write(record, json.dumps(intent).encode())
    if action == 'prepare':
        write(root / 'asterion_node_agent', (stage / 'asterion_node_agent').read_bytes(), 0o700)
    elif action == 'uninstall':
        (root / 'asterion_node_agent').unlink(missing_ok=True)
        (root / 'install.json').unlink(missing_ok=True)
        if not preserve:
            for file in state.iterdir():
                if file.name in ('logs', 'agent.lock'):
                    continue
                if file.is_dir() and not file.is_symlink():
                    shutil.rmtree(file)
                else:
                    file.unlink()
            for name in ('ca.crt', 'server.crt', 'server.key', 'install.json'):
                (root / name).unlink(missing_ok=True)
        sync(root)
    else:
        raise ValueError('Invalid remote maintenance action')
finally:
    os.close(lock)
if action == 'prepare':
    result = subprocess.run([str(root / 'asterion_node_agent'), '--directory', str(state),
        '--prepare-programs', str(stage / 'programs.pb'), '--bind', bind, '--port', port],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise ValueError('Agent program preparation failed; inspect ' + str(state / 'logs'))
    installed = json.loads((root / 'install.json').read_text())
    installed['version'] = version
    write(root / 'install.json', json.dumps(installed).encode())
intent['complete'] = True
write(record, json.dumps(intent).encode())
print(json.dumps(dict(complete=True)))
)PY";

} // namespace asterion::terminal
