"""The real acceptance scripts run against isolated Market and Trading fixtures."""
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).parents[1]/'support'))
from warehouse_installation import market_host
from trading_installation import Programs, install
import warehouse_tls
build=Path(sys.argv[1]).resolve()
script=Path(__file__).parents[1]/'acceptance/ctp_market.py'
year = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).year + 1
contract = f'rb{year % 100:02d}10'
with tempfile.TemporaryDirectory(prefix='ast-market-acceptance-test-') as folder:
    with market_host(build,Path(folder)/'market',user='acceptance') as port:
        tls = Path(folder) / 'market/config/tls'
        warehouse_tls.install_client(tls, Path(folder) / 'client')
        for symbol, success in [(contract,True),('rb2999',False)]:
            report=Path(folder)/(symbol+'.json')
            result=subprocess.run([sys.executable,str(script),'--build',str(build),'--host','127.0.0.1',
                '--port',str(port),'--fingerprint',warehouse_tls.fingerprint(tls),
                '--client-identity',str(Path(folder)/'client/terminal/data/tls'),'--instrument','SHFE:'+symbol,'--timeout','4','--report',str(report)],
                capture_output=True,text=True,timeout=30)
            assert result.returncode==(0 if success else 1),result.stdout+result.stderr
            assert json.loads(report.read_text())['passed'] is success
            assert 'fixture-only-secret' not in report.read_text()+result.stdout+result.stderr
        if sys.platform == 'linux':
            client = Path(folder) / 'client'
            connections = client / 'terminal/data/trading-connections'
            connections.mkdir()
            host = install(Path(folder) / 'trading', Programs(
                str(build / 'asterion_trading'), str(build / 'asterion_test_certificates'),
                str(build / 'libasterion_test_ctp_trader.so')))
            script = Path(__file__).parents[1] / 'acceptance/ctp_trading.py'
            for user, success in [('resting', True), ('strategy', False)]:
                trading_port = host.account(user, terminals=[warehouse_tls.fingerprint(tls, 'reader')], user=user)
                (connections / (user + '.json')).write_text(json.dumps(dict(
                    id=user, host='127.0.0.1', port=trading_port, instance=user,
                    fingerprint=host.server, revision='test')))
                service = host.start(user)
                observer = None
                try:
                    report = Path(folder) / (user + '.json')
                    result = subprocess.run([
                        sys.executable, str(script), '--simulation', '--build', str(build),
                        '--terminal-directory', str(client / 'terminal'), '--trading-connection', user,
                        '--market-host', '127.0.0.1', '--market-port', str(port),
                        '--market-fingerprint', warehouse_tls.fingerprint(tls),
                        '--trade-front', 'tcp://127.0.0.1:41205', '--broker', '9999', '--user', user,
                        '--instrument', 'SHFE:' + contract, '--timeout', '5', '--report', str(report)],
                        capture_output=True, text=True, timeout=60)
                    outcome = json.loads(report.read_text())
                    assert result.returncode == (0 if success else 1), result.stdout + result.stderr
                    assert outcome['passed'] is success, outcome
                    assert any(step['step'] == 'authorization revoked' and step['ok']
                               for step in outcome['steps']), outcome
                    observer = subprocess.Popen([str(build / 'asterion_terminal_dev_bridge')],
                        env=dict(os.environ, ASTERION_ROOT=str(client), ASTERION_TEST_NODE_ISOLATED='1'),
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

                    def trading(request):
                        observer.stdin.write(json.dumps(dict(id=1, request=dict(version=1,
                            method='trading.call', params=dict(account=user, request=request)))) + '\n')
                        observer.stdin.flush()
                        reply = json.loads(observer.stdout.readline())['reply']
                        assert 'error' not in reply, reply
                        return reply['result']['trading']

                    live = trading({'getAccount': {}})['getAccount']['session']
                    assert 'authorizedMs' not in live and not live.get('workingOrders'), live
                    assert live['brokerState'] == 'BROKER_STATE_DISCONNECTED', live
                    orders = trading({'getOrders': {}})['getOrders']['orders']
                    assert len(orders) == 1, orders
                    assert orders[0]['state'] == ('ORDER_STATE_CANCELLED' if success else 'ORDER_STATE_FILLED'), orders
                    if not success:
                        assert outcome['position_kept'] == '1', outcome
                    for secret in ('fixture-only-secret', 'test-only-password', 'test-only-auth'):
                        assert secret not in report.read_text() + result.stdout + result.stderr
                    for line in result.stdout.splitlines():
                        if line.startswith('node kept for its logs: '):
                            shutil.rmtree(Path(line.split(': ', 1)[1]).parents[1])
                finally:
                    if observer:
                        observer.communicate(timeout=15)
                    service.terminate()
                    service.wait(timeout=15)
