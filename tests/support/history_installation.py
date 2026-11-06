"""An isolated, independently supervised History with explicit test-only credentials."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time
import history_tls

def install_program(binary, installed):
    binary, installed = Path(binary), Path(installed)
    # Debug binaries include DuckDB and can exceed 600 MiB. Parallel isolated
    # hosts share immutable program bytes without consuming the /tmp quota;
    # each host still owns its configuration, credentials, logs and data.
    if binary.stat().st_dev == installed.parent.stat().st_dev:
        os.link(binary, installed)
    else:
        shutil.copy2(binary, installed)

@contextmanager
def history_host(build, root, *, vendor=False, exchanges=(), user="steady"):
    root = Path(root)
    for name in ('bin','lib','config','logs','data','credentials'):
        (root / name).mkdir(parents=True)
    binary = build / 'asterion_history'
    installed = root / 'bin/asterion_history'
    install_program(binary, installed)
    if sys.platform == "linux":
        install_program(build / "asterion_crash_reporter", root / "bin/asterion_crash_reporter")
    suffix = '.dylib' if sys.platform == 'darwin' else '.so'
    for installed, fixture, real in [('ctp-md', 'libasterion_test_ctp', 'ctp-md'),
                                     ('ctp-trader', 'libasterion_test_ctp_trader', 'ctp-trader')]:
        shutil.copy2(build / ((real if vendor else fixture) + suffix), root / 'lib' / (installed+'.so'))
    shutil.copy2(Path(__file__).parents[2] / 'apps/services/history/config/logging.json', root/'config/logging.json')
    with socket.socket() as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    tls = history_tls.prepare(root)
    (root/'config/history.json').write_text(json.dumps({'bind':'127.0.0.1','port':port, 'data':str(root/'data'), 'clients':history_tls.clients(tls), 'acquisition':{'exchanges':list(exchanges), 'tushare':None},
        'counter':{'market_front':'tcp://127.0.0.1:1','trade_front':'tcp://127.0.0.1:1',
                   'broker':'test','user':user,'app_id':'app'}}))
    (root/'credentials/asterion.ctp.password').write_text('fixture-only-secret')
    (root/'credentials/asterion.ctp.auth_code').write_text('fixture-auth')
    with running_market(root, port) as process:
        yield port
        assert process.poll() is None, "a client stopped the independent History"


@contextmanager
def running_market(root, port):
    """Restart an existing isolated installation; the caller may test abrupt exits."""
    log = (root/'stderr').open('a')
    process = subprocess.Popen([root/'bin/asterion_history'],
        env=dict(os.environ,CREDENTIALS_DIRECTORY=str(root/'credentials')),stdout=subprocess.DEVNULL,stderr=log)
    try:
        for _ in range(200):
            if process.poll() is not None:
                raise AssertionError((root/'stderr').read_text())
            try:
                with socket.create_connection(('127.0.0.1',port),timeout=.1):break
            except OSError:time.sleep(.05)
        else:raise AssertionError('History did not listen')
        yield process
    finally:
        process.terminate()
        try:process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill();process.wait();raise
        log.close()


if __name__ == "__main__":
    # A desktop test owns this host, independently of the Electron application.
    with history_host(Path(sys.argv[1]), Path(sys.argv[2])) as port:
        print(port, flush=True)
        sys.stdin.read()
