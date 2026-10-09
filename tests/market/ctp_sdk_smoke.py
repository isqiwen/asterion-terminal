"""Check both vendor ABIs and loopback lifecycles using test-only credentials."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
build = Path(sys.argv[1]).resolve()
if sys.platform != 'darwin' and not sys.platform.startswith('linux'):
    raise SystemExit('Vendor SDK acceptance requires macOS or Linux')
suffix = '.dylib' if sys.platform == 'darwin' else '.so'
library = Path(os.environ.get('ASTERION_CTP_LIBRARY', str(build / ('ctp-md' + suffix))))
trader_library = Path(os.environ.get('ASTERION_CTP_CATALOG_LIBRARY', str(build / ('ctp-trader' + suffix))))
for sdk_path in (library, trader_library):
    if not sdk_path.is_file():
        raise SystemExit('Required vendor SDK is missing: ' + str(sdk_path))
import ctypes
sdk = ctypes.CDLL(str(library))
name = '_ZN15CThostFtdcMdApi13GetApiVersionEv'
version = getattr(sdk, name);version.restype = ctypes.c_char_p
assert b'6.7.7' in version(), version()
print('Packaged CTP vendor SDK loaded, version:', version().decode())
import json
bridge = build / 'asterion_terminal_dev_bridge'
env = dict(os.environ, ASTERION_CTP_LIBRARY=str(library))
process = subprocess.Popen([str(bridge)], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
def call(method, params=None):
    process.stdin.write(json.dumps(dict(version=1, method=method, params=params or {}))+'\n');process.stdin.flush()
    response=json.loads(process.stdout.readline());assert 'error' not in response,response
    return response['result']
try:
    initial=call('market.local')
    service=next(s for n in initial['nodes'] for s in n['health']['services'] if s['kind']=='market')
    stopped=call('node.action',dict(id='local',service=service['id'],action='stop'))
    current=next(s for n in stopped['nodes'] for s in n['health']['services'] if s['id']==service['id'])
    updated=call('node.update',dict(id='local',service=service['id'],revision=current['revision']))
    assert next(s for n in updated['nodes'] for s in n['health']['services'] if s['id']==service['id'])['state']=='stopped'
    call('node.action',dict(id='local',service=service['id'],action='start'))
    call('market.attach',dict(id='local',service=service['id']))
    call('ctp.connections.save',dict(id='fixture',name='Fixture',revision='',broker_id='test',user_id='fixture',app_id='app',trade_front='tcp://127.0.0.1:1',market_front='tcp://127.0.0.1:1'))
    call('market.connect',dict(password='loopback-only',instruments=[]))
    time.sleep(.2)
    assert call('market.disconnect')['market']['phase']=='disconnected'
finally:
    process.terminate();process.communicate(timeout=8)
subprocess.run([str(build / 'asterion_ctp_trader_tests'),
                '--gtest_filter=CtpVendor.TraderLoopbackLifecycleUsesThePackagedAbi'],
               env=dict(os.environ, ASTERION_VENDOR_CTP_TRADER_LIBRARY=str(trader_library)),
               check=True, timeout=20)
print('Vendor market and trader factories, loopback startup, reconnect and Release passed; no external server contacted')
