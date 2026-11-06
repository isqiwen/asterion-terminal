"""The remote Market acceptance path passes on quotes and fails on missing data."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).parents[1]/'support'))
from history_installation import quotes_host
import history_tls
build=Path(sys.argv[1]).resolve()
script=Path(__file__).parents[1]/'acceptance/ctp_market.py'
with tempfile.TemporaryDirectory(prefix='ast-market-acceptance-test-') as folder:
    with quotes_host(build,Path(folder)/'market') as port:
        tls = Path(folder) / 'market/config/tls'
        history_tls.install_client(tls, Path(folder) / 'client')
        for symbol, success in [('rb2610',True),('rb2999',False)]:
            report=Path(folder)/(symbol+'.json')
            result=subprocess.run([sys.executable,str(script),'--build',str(build),'--host','127.0.0.1',
                '--port',str(port),'--fingerprint',history_tls.fingerprint(tls),
                '--client-identity',str(Path(folder)/'client/terminal/data/tls'),'--instrument','SHFE:'+symbol,'--timeout','4','--report',str(report)],
                capture_output=True,text=True,timeout=30)
            assert result.returncode==(0 if success else 1),result.stdout+result.stderr
            assert json.loads(report.read_text())['passed'] is success
            assert 'fixture-only-secret' not in report.read_text()+result.stdout+result.stderr
