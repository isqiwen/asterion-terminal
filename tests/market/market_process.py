"""Terminal connects to an independent Market; closing the desktop cannot own its lifecycle."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
build = Path(sys.argv[1]).resolve()
port = int(sys.argv[2])
sys.path.insert(0, str(Path(__file__).parents[1] / 'support'))
import market_tls
tls = Path(sys.argv[3])
market_tls.install_client(tls, os.environ['ASTERION_ROOT'])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'terminal'))
from keychain_fixture import install
env = dict(os.environ, ASTERION_KEYCHAIN_EXECUTABLE=install(Path(os.environ['ASTERION_ROOT']) / 'market-test-keychain'))
def terminal():
    return subprocess.Popen([str(build/'asterion_terminal_dev_bridge')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
def call(process, method, params=None):
    process.stdin.write(json.dumps(dict(version=1,method=method,params=params or {}))+'\n');process.stdin.flush()
    reply=json.loads(process.stdout.readline());assert 'error' not in reply,reply;return reply['result']
def ready(process):
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        state=call(process,'runtime.snapshot')
        if state.get('market') and state['market']['transport_online'] and state['market']['subscriptions'][0]['quote']:
            return state
        time.sleep(.1)
    raise AssertionError(state)
def close(process):
    process.stdin.close();process.wait(timeout=8)
    assert process.returncode == 0, process.stderr.read()
process=terminal()
try:
    call(process,'market.endpoint.save',dict(host='127.0.0.1',port=port,fingerprint=market_tls.fingerprint(tls)))
    call(process,'market.open')
    state=ready(process)
    instance=state['market']['instance_id']
    recording=call(process,'market.recording')['recording']
    assert recording['instance']==instance and recording['phase']=='connected',recording
    assert recording['subscribed']==1 and int(recording['lastQuoteMs'])>0,recording
    assert state['market']['remote'] and state['market']['subscriptions'][0]['quote']['last']=='3510'
    assert all(service['kind']!='market' for node in state['nodes'] for service in (node.get('health') or {}).get('services',[])), state['nodes']
    selected=[dict(venue='SHFE',symbol='rb2610')]
    state=call(process,'market.watchlist',dict(instruments=selected))
    assert state['market']['watchlist']==selected
    assert call(process,'market.close')['market'] is None
    call(process,'market.open')
    assert ready(process)['market']['instance_id']==instance
    assert call(process,'market.recording')['recording']['instance']==instance
    close(process)
    process=terminal()
    call(process,'market.open')
    state=ready(process)
    assert state['market']['instance_id']==instance and state['market']['watchlist']==selected
    assert 'fixture-only-secret' not in json.dumps(state)
    call(process,'market.endpoint.clear')
    assert call(process,'runtime.snapshot')['market'] is None
finally:
    if process.poll() is None:
        close(process)
print('Terminal reconnects to the same remote Market, owns only its saved watchlist, and starts no local Market')
