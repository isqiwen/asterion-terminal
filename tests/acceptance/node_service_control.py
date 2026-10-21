"""Opt-in native launchd ownership/stop/restart acceptance using a unique label."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build',type=Path,required=True)
parser.add_argument('--allow-user-service',action='store_true',required=True)
args=parser.parse_args()
if sys.platform!='darwin':raise SystemExit('Native launchd acceptance requires macOS')
build=args.build.resolve();name='me.asterion.acceptance.'+uuid.uuid4().hex[:12]
domain=f'gui/{os.getuid()}/{name}'
def wait(check):
    end=time.monotonic()+15
    while time.monotonic()<end:
        if check():return
        time.sleep(.1)
    raise AssertionError('native service condition timed out')
def alive(pid):
    try:os.kill(pid,0);return True
    except ProcessLookupError:return False
with tempfile.TemporaryDirectory(prefix='ast-os-',dir='/tmp', ignore_cleanup_errors=True) as folder:
    root=Path(folder).resolve();home=root/'home';home.mkdir();state=root/'state';state.mkdir();bin_dir=state/'bin';bin_dir.mkdir()
    binary=bin_dir/'asterion_node_agent';shutil.copy2(build/'asterion_node_agent',binary)
    shutil.copytree(build / "config", state / "config")
    endpoint=str(root/'agent.sock');env=dict(os.environ,HOME=str(home))
    base=[str(build/'asterion_test_node_service_control'),'--executable',str(binary),'--root',str(state),'--endpoint',endpoint,'--name',name]
    def run(operation,pid=0,extra=None):
        command=base+['--operation',operation,'--pid',str(pid)]
        if extra:
            for key,value in extra.items():
                if key in command:command[command.index(key)+1]=value
                else:command.extend([key,value])
        return subprocess.run(command,env=env,capture_output=True,text=True,timeout=30)
    def pid():return int((state/'agent.pid').read_text())
    try:
        started=run('install');assert started.returncode==0,started.stderr
        wait(lambda:(state/'agent.pid').exists() and alive(pid()))
        assert run('verify-stopped').returncode!=0
        first=pid();definition=home/'Library/LaunchAgents'/f'{name}.plist';original=definition.read_bytes()
        marker=state/'preserved-data';marker.write_text('retain this test-owned data')
        deployed=run('deploy-market',extra={'--source':str(build/'asterion_market_data'),
                                           '--provider':str(build/'libasterion_test_ctp.dylib')})
        assert deployed.returncode==0,deployed.stderr
        health=json.loads(run('status').stdout)['health']
        children=[service['pid'] for service in health['services'] if service['pid']]
        assert children
        configurations={path:path.read_bytes() for path in (state/'services').glob('*/service.json')}
        assert run('stop',os.getpid()).returncode!=0
        assert alive(first)
        assert run('stop',first,{'--endpoint':endpoint+'-wrong'}).returncode!=0
        assert alive(first) and definition.read_bytes()==original
        definition.write_bytes(original+b'\n')
        assert run('stop',first).returncode!=0
        assert alive(first)
        definition.write_bytes(original)
        stopped=run('stop',first);assert stopped.returncode==0,stopped.stderr
        wait(lambda:not alive(first))
        wait(lambda:all(not alive(child) for child in children))
        assert all(path.read_bytes()==contents for path,contents in configurations.items())
        assert marker.read_text()=='retain this test-owned data' and definition.read_bytes()==original
        assert subprocess.run(['/bin/launchctl','print',domain],capture_output=True).returncode!=0
        verified=run('verify-stopped');assert verified.returncode==0,verified.stderr
        # A matching on-disk definition does not prove the loaded registration
        # belongs to that file. Reject a service loaded from another location.
        alternate=home/'alternate.plist';alternate.write_bytes(original)
        foreign=subprocess.run(['/bin/launchctl','bootstrap',f'gui/{os.getuid()}',str(alternate)],capture_output=True)
        assert foreign.returncode==0,foreign.stderr
        wait(lambda:pid()!=first and alive(pid()))
        foreign_pid=pid()
        assert run('stop',foreign_pid).returncode!=0
        assert alive(foreign_pid) and marker.exists()
        removed=subprocess.run(['/bin/launchctl','bootout',domain],capture_output=True)
        assert removed.returncode==0,removed.stderr
        wait(lambda:not alive(foreign_pid))
        restarted=run('install');assert restarted.returncode==0,restarted.stderr
        wait(lambda:pid()!=first and alive(pid()))
        stopped=run('stop',pid());assert stopped.returncode==0,stopped.stderr
        # Startup recovery must remain stoppable through the same owned OS service path.
        pending=state/'services/market-running/service.pending'
        pending.write_bytes(b'test-owned unfinished configuration')
        configurations={path:path.read_bytes() for path in (state/'services').glob('*/service.json')}
        installed=run('install');assert installed.returncode==0,installed.stderr
        wait(lambda:(response:=run('status')).returncode==0 and
             json.loads(response.stdout)['health']['phase']=='recovery_required')
        recovering=pid()
        stopped=run('stop',recovering);assert stopped.returncode==0,stopped.stderr
        wait(lambda:not alive(recovering))
        assert pending.read_bytes()==b'test-owned unfinished configuration'
        assert all(path.read_bytes()==contents for path,contents in configurations.items())
        assert marker.read_text()=='retain this test-owned data'
    finally:
        subprocess.run(['/bin/launchctl','bootout',domain],capture_output=True)
print('Unique launchd registration: wrong PID/config/registration rejected, owned stop confirmed in running and recovery phases, children stopped and configurations retained, restart passed')
