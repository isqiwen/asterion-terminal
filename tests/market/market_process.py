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
env = dict(os.environ)
def terminal():
    return subprocess.Popen([str(build/'asterion_terminal_dev_bridge')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
def call(process, method, params=None):
    process.stdin.write(json.dumps(dict(version=1,method=method,params=params or {}))+'\n');process.stdin.flush()
    reply=json.loads(process.stdout.readline());assert 'error' not in reply,reply;return reply['result']
def ready(process):
    """The open watch's latest frame once its row has a quote."""
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        watch=call(process,'market.watch',dict(restart=False))['market_watch']
        for frame in reversed(watch['frames']):
            rows=frame.get('subscriptions',[])
            if watch['online'] and rows and 'quote' in rows[0]:
                return frame
        time.sleep(.1)
    raise AssertionError(watch)
def recording(process):
    return call(process,'market.call',dict(request=dict(recordingStatus={})))['market']['recordingStatus']
def close(process):
    process.stdin.close();process.wait(timeout=8)
    assert process.returncode == 0, process.stderr.read()
process=terminal()
try:
    call(process,'market.endpoint.save',dict(host='127.0.0.1',port=port,fingerprint=market_tls.fingerprint(tls)))
    call(process,'market.open')
    frame=ready(process)
    instance=frame['instanceId']
    status=recording(process)
    assert status['instance']==instance and status['phase']=='connected',status
    assert status['subscribed']==1 and int(status['lastQuoteMs'])>0,status
    assert frame['subscriptions'][0]['quote']['last']=='3510'
    state=call(process,'runtime.snapshot')
    assert 'market' not in state
    selected=[dict(venue='SHFE',symbol='rb2610')]
    state=call(process,'market.watchlist',dict(instruments=selected))
    assert state['market_endpoint']['instruments']==selected
    call(process,'market.close')
    assert call(process,'market.watch',dict(restart=False))['market_watch'] is None
    call(process,'market.open')
    assert ready(process)['instanceId']==instance
    close(process)
    process=terminal()
    call(process,'market.open')
    frame=ready(process)
    assert frame['instanceId']==instance
    assert call(process,'runtime.snapshot')['market_endpoint']['instruments']==selected
    assert 'fixture-only-secret' not in json.dumps(frame)
    call(process,'market.endpoint.clear')
    assert call(process,'market.watch',dict(restart=False))['market_watch'] is None
finally:
    if process.poll() is None:
        close(process)
print('Terminal reconnects to the same remote Market, owns only its saved watchlist, and starts no local Market')
