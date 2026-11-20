"""Terminal connects to an independent Warehouse; closing the desktop cannot own its lifecycle."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
build = Path(sys.argv[1]).resolve()
port = int(sys.argv[2])
sys.path.insert(0, str(Path(__file__).parents[1] / 'support'))
import warehouse_tls
tls = Path(sys.argv[3])
warehouse_tls.install_client(tls, os.environ['ASTERION_ROOT'])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'terminal'))
env = dict(os.environ)
def terminal():
    return subprocess.Popen([str(build/'asterion_terminal_dev_bridge')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
def call(process, method, params=None):
    process.stdin.write(json.dumps(dict(id=1,request=dict(version=1,method=method,params=params or {})))+'\n');process.stdin.flush()
    reply=json.loads(process.stdout.readline())['reply'];assert 'error' not in reply,reply;return reply['result']
def ready(process):
    """The open watch's latest frame once its row has a quote."""
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        watch=call(process,'market.watch',dict(restart=False))['market_watch']
        for frame in reversed(watch['frames']):
            frame = frame.get('full', frame.get('changed', {}))
            rows=frame.get('rows',[])
            if watch['online'] and rows and 'quote' in rows[0]:
                return frame
        time.sleep(.1)
    raise AssertionError(watch)
def status(process):
    return call(process,'market.call',dict(request=dict(getStatus={})))['market']['getStatus']
def close(process):
    process.stdin.close();process.wait(timeout=8)
    assert process.returncode == 0, process.stderr.read()
process=terminal()
try:
    call(process,'market.endpoint.save',dict(host='127.0.0.1',port=port,fingerprint=warehouse_tls.fingerprint(tls)))
    call(process,'market.open')
    frame=ready(process)
    started=status(process)
    assert started['feed']['state']=='FEED_STATE_CONNECTED',started
    assert started['subscribed']==1 and int(started['lastQuoteMs'])>0,started
    assert frame['rows'][0]['quote']['last']['text']=='3510'
    state=call(process,'runtime.snapshot')
    assert 'market' not in state
    selected=[dict(exchange='SHFE',symbol='rb2610')]
    state=call(process,'market.watchlist',dict(instruments=selected))
    assert state['market_endpoint']['instruments']==selected
    call(process,'market.close')
    assert call(process,'market.watch',dict(restart=False))['market_watch'] is None
    call(process,'market.open')
    assert ready(process)['rows'][0]['quote']['last']['text']=='3510'
    close(process)
    process=terminal()
    call(process,'market.open')
    frame=ready(process)
    assert status(process)['startedMs']==started['startedMs']
    assert call(process,'runtime.snapshot')['market_endpoint']['instruments']==selected
    assert 'fixture-only-secret' not in json.dumps(frame)
    call(process,'market.endpoint.clear')
    assert call(process,'market.watch',dict(restart=False))['market_watch'] is None
finally:
    if process.poll() is None:
        close(process)
print('Terminal reconnects to the same remote Warehouse, owns only its saved watchlist, and starts no local Warehouse')
