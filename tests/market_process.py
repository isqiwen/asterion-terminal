"""Agent-owned live-market process integration with an explicitly test-only SDK."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
build=Path(sys.argv[1]).resolve()
env=dict(os.environ,ASTERION_CTP_LIBRARY=str(build/('asterion_test_ctp.dll' if sys.platform=='win32' else 'libasterion_test_ctp.dylib' if sys.platform=='darwin' else 'libasterion_test_ctp.so')))
env['ASTERION_CTP_CATALOG_LIBRARY']=str(build/('asterion_test_ctp_trader.dll' if sys.platform=='win32' else 'libasterion_test_ctp_trader.dylib' if sys.platform=='darwin' else 'libasterion_test_ctp_trader.so'))
process=subprocess.Popen([str(build/('asterion_terminal_dev_bridge.exe' if sys.platform=='win32' else 'asterion_terminal_dev_bridge'))],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
def call(method,params=None):
    process.stdin.write(json.dumps(dict(version=1,method=method,params=params or {}))+'\n');process.stdin.flush()
    reply=json.loads(process.stdout.readline());assert 'error' not in reply,reply;return reply['result']
def wait(predicate):
    deadline=time.monotonic()+12
    while time.monotonic()<deadline:
        state=call('runtime.snapshot')
        if predicate(state):return state
        time.sleep(.1)
    raise AssertionError(state)
try:
    state=call('market.local');assert state['market']['phase']=='disconnected'
    call('ctp.connections.save',dict(id='fixture',name='Fixture',revision='',broker_id='test',user_id='fixture',app_id='app',trade_front='tcp://127.0.0.1:1',market_front='tcp://127.0.0.1:1'))
    call('ctp.connections.save',dict(id='other',name='Other',revision='',broker_id='test',user_id='other',app_id='app',trade_front='tcp://127.0.0.1:1',market_front='tcp://127.0.0.1:1'))
    state=call('market.connect',dict(password='fixture-only-secret',instruments=[dict(venue='SHFE',symbol='rb2610'),dict(venue='SHFE',symbol='bad2601')]))
    state=wait(lambda s:s['market']['phase']=='connected' and s['market']['subscriptions'][0]['quote'])
    market=state['market'];quote=market['subscriptions'][0]['quote'];assert quote['last']=='3510' and quote['bid']=='3509' and quote['ask']=='3511'
    assert quote['previous_settlement'] is None and market['out_of_order']>=1 and market['subscriptions'][1]['state']=='error'
    assert 'fixture-only-secret' not in json.dumps(state) and 'password' not in json.dumps(state)
    state=wait(lambda s:s['market']['history']['available'] and len(s['market']['history']['points'])>0)
    points=state['market']['history']['points']
    assert all(p['price']=='3510' and p['symbol']=='rb2610' for p in points),points
    assert all('volume' not in p for p in points)
    wait(lambda s:s['market']['phase']=='reconnecting')
    wait(lambda s:s['market']['phase']=='connected' and s['market']['out_of_order']>=2)
    # The market data account cannot change under a live market login.
    process.stdin.write(json.dumps(dict(version=1,method='ctp.connections.market',params=dict(id='other')))+'\n');process.stdin.flush()
    refused=json.loads(process.stdout.readline());assert 'disconnect market data' in refused['error']['message'],refused
    call('market.subscribe',dict(instruments=[dict(venue='SHFE',symbol='rb2610')]))
    assert len(wait(lambda s:len(s['market']['subscriptions'])==1)['market']['subscriptions'])==1
    call('market.catalog',dict(account='fixture',password='fixture-only-secret',auth_code='fixture-auth'))
    state=wait(lambda s:s['market']['catalog']['phase']=='ready')
    assert state['market']['catalog']['contracts'][0]['symbol']=='rb2610'
    call('market.subscribe',dict(instruments=[]))
    state=wait(lambda s:not s['market']['watchlist'])
    assert len(state['market']['subscriptions'])==1
    state=call('market.disconnect');state=wait(lambda s:s['market']['phase']=='disconnected')
    # The last ready catalog stays available offline, marked as a cached copy.
    catalog=state['market']['catalog'];assert catalog['phase']=='cached' and catalog['contracts'][0]['symbol']=='rb2610',catalog
    call('market.connect',dict(password='reject-test-only',instruments=[]))
    wait(lambda s:s['market']['phase']=='error' and s['market']['error_code']==3)
    call('market.disconnect')
    call('node.local')
    state=wait(lambda s:any(n['health'] and any(v['kind']=='market' for v in n['health']['services']) for n in s['nodes']))
    # Agent heartbeat remains healthy when the feed is deliberately disconnected.
    wait(lambda s:any(n['health'] and any(v['kind']=='market' and v['health']=='awaiting_input' for v in n['health']['services']) for n in s['nodes']))
finally:
    process.terminate();stdout,stderr=process.communicate(timeout=8)
    assert 'fixture-only-secret' not in stdout+stderr
print('Market login, subscription rejection, typed quote push, reconnect, ordering, credential redaction and Agent health passed')
