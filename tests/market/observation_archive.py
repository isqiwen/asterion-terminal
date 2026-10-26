"""Real Agent/Market/Data processes, exclusively isolated synthetic CTP observations."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
assert os.environ.get("ASTERION_TEST_NODE_ISOLATED") == "1"
build = Path(sys.argv[1]).resolve()
suffix = ".dylib" if sys.platform == "darwin" else ".so"
env = dict(os.environ, ASTERION_CTP_LIBRARY=str(build / ("libasterion_test_ctp" + suffix)))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "terminal"))
from keychain_fixture import install
env["ASTERION_KEYCHAIN_EXECUTABLE"] = install(Path(env["ASTERION_NODE_DIRECTORY"]) / "archive-keychain")
process = subprocess.Popen([str(build / "asterion_terminal_dev_bridge")], env=env,
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
def call(method, params=None, error=False):
    process.stdin.write(json.dumps(dict(version=1, method=method, params=params or {}))+"\n")
    process.stdin.flush()
    reply=json.loads(process.stdout.readline())
    assert ("error" in reply) == error, reply
    return reply["error"] if error else reply["result"]
def wait(predicate):
    end=time.monotonic()+12
    while time.monotonic()<end:
        snapshot=call("runtime.snapshot")
        if predicate(snapshot): return snapshot
        time.sleep(.05)
    raise AssertionError(snapshot)
try:
    call("market.local")
    call("ctp.connections.save",dict(id="archive-fixture",name="Archive fixture",revision="",broker_id="test",user_id="fixture",app_id="app",trade_front="tcp://127.0.0.1:1",market_front="tcp://127.0.0.1:1"))
    call("market.connect",dict(password="synthetic-archive-secret",instruments=[dict(venue="SHFE",symbol="rb2610")]))
    wait(lambda s:s["market"]["phase"]=="connected" and s["market"]["out_of_order"]>=2)
    preview=call("market.observation.preview",dict(venue="SHFE",symbol="rb2610"))["market_observation"]
    assert preview["aggregate_only"] and not preview["complete_history"],preview
    assert preview["minutes"]["first_observation_ms"]>0 and preview["minutes"]["interrupted"],preview
    call("node.data.local.open")
    wait(lambda s:s["data"]["online"])
    admission=dict(id="fixed-observation",source=preview["reference"])
    first=call("data.observation.admit",admission)["observation_archive"]
    assert first["state"]=="admitted" and "capture" not in first,first
    assert call("data.observation.admit",admission)["observation_archive"]==first
    wrong=dict(preview["reference"],node="different-node")
    assert call("data.observation.admit",dict(id="foreign",source=wrong),error=True)["code"]=="conflict"
    different=dict(preview["reference"],version="0"*64)
    assert call("data.observation.admit",dict(id="fixed-observation",source=different),error=True)["code"]=="conflict"
    published=call("data.observation.execute",dict(id="fixed-observation"))["observation_archive"]
    assert published["state"]=="published",published
    original=call("data.observation.read",dict(id="fixed-observation"))["observation_archive"]
    assert original["capture"]==preview,original
    assert original["archive"]["version"]==preview["reference"]["version"]
    lost=call("market.observation.preview",dict(venue="SHFE",symbol="rb2610"))["market_observation"]
    call("data.observation.admit",dict(id="missing-original",source=lost["reference"]))
    for _ in range(8): call("market.observation.preview",dict(venue="SHFE",symbol="rb2610"))
    assert call("data.observation.execute",dict(id="missing-original"),error=True)["code"]=="not_found"
    pending=call("data.observation.read",dict(id="missing-original"))["observation_archive"]
    assert pending["state"]=="admitted" and pending["source"]==lost["reference"] and "capture" not in pending
    call("node.action",dict(id="local",service=preview["reference"]["service"],action="stop"))
    assert call("data.observation.execute",dict(id="fixed-observation"))["observation_archive"]==published
    assert call("data.observation.read",dict(id="fixed-observation"))["observation_archive"]==original
    call("node.action",dict(id="local",service=original["archive"]["service"],action="stop"))
    call("node.data.local.open")
    wait(lambda s:s["data"]["online"])
    assert call("data.observation.read",dict(id="fixed-observation"))["observation_archive"]==original
    assert call("data.observation.read",dict(id="missing-original"))["observation_archive"]==pending
finally:
    process.terminate()
    stdout,stderr=process.communicate(timeout=10)
    assert "synthetic-archive-secret" not in stdout+stderr
print("Market fixed aggregate -> Data admission/publication -> independent read/restart, exact loss and owner refusal passed")
