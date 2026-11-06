"""Read-only acceptance against an independently deployed Market.

Use --host, --port, --fingerprint, --client-identity and --instrument.
Provider credentials are configured on the Market host. Closing this test must
leave the same service instance collecting and serving other clients.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', default=str(Path(__file__).parents[2] / 'build/Debug'))
parser.add_argument('--host', required=True)
parser.add_argument('--port', type=int, default=7450)
parser.add_argument('--fingerprint', required=True, help='Market server certificate fingerprint')
parser.add_argument('--client-identity', required=True, help='Directory containing registered client.crt and client.key')
parser.add_argument('--instrument', action='append', required=True, help='VENUE:SYMBOL')
parser.add_argument('--timeout', type=int, default=90)
parser.add_argument('--report', default='')
args = parser.parse_args()
wanted = set(args.instrument)
if any(len(key.split(':')) != 2 or not all(key.split(':')) for key in wanted):
    parser.error('instruments must be VENUE:SYMBOL')
report = {'host':args.host,'port':args.port,'instruments':args.instrument,'passed':False,'steps':[]}
def step(name, ok, detail=''):
    report['steps'].append({'step':name,'ok':ok,'detail':detail})
    print(('ok: ' if ok else 'FAIL: ')+name+' '+detail)
with tempfile.TemporaryDirectory(prefix='asterion-history-acceptance-') as root:
    target = Path(root) / 'terminal/data/tls'
    target.mkdir(parents=True, mode=0o700)
    for name in ('client.crt', 'client.key'):
        shutil.copy2(Path(args.client_identity) / name, target / name)
    (target / 'client.key').chmod(0o600)
    core = subprocess.Popen([str(Path(args.build)/'asterion_terminal_dev_bridge')],
        env=dict(os.environ,ASTERION_ROOT=root,ASTERION_TEST_NODE_ISOLATED='1'),
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True)
    def call(method, params=None):
        core.stdin.write(json.dumps({'version':1,'method':method,'params':params or {}})+'\n');core.stdin.flush()
        response=json.loads(core.stdout.readline())
        if 'error' in response:raise RuntimeError(response['error']['message'])
        return response['result']
    try:
        call('quotes.endpoint.save',{'host':args.host,'port':args.port,'fingerprint':args.fingerprint})
        def instance():
            return call('quotes.call',{'request':{'getStatus':{}}})['quotes']['getStatus']['startedMs']
        call('quotes.open')
        original=instance()
        seen=set();deadline=time.monotonic()+args.timeout
        while time.monotonic()<deadline:
            for frame in call('quotes.watch',{'restart':False})['quotes_watch']['frames']:
                value = frame.get('full', frame.get('changed', {}))
                for row in value.get('rows',[]):
                    key=row['instrument']['exchange']+':'+row['instrument']['symbol']
                    if key in wanted and row.get('quote',{}).get('last') is not None:
                        seen.add(key)
            if wanted<=seen:break
            time.sleep(.2)
        step('requested instruments quoted',wanted<=seen,','.join(sorted(seen)))
        call('quotes.close')
        step('client closes',call('quotes.watch',{'restart':False})['quotes_watch'] is None)
        call('quotes.open')
        step('same service continues',instance()==original)
    except Exception as error:
        step('run',False,str(error))
    finally:
        core.stdin.close()
        try:core.wait(timeout=15)
        except subprocess.TimeoutExpired:
            core.kill();core.wait();step('Terminal cleanup',False)
    report['passed']=bool(report['steps']) and all(item['ok'] for item in report['steps'])
if args.report:Path(args.report).write_text(json.dumps(report,ensure_ascii=False,indent=2))
sys.exit(0 if report['passed'] else 1)
