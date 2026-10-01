"""Production strategy host drives a separate paper service over TCP/mTLS."""
from pathlib import Path
import hashlib
import json
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import threading
import os

scheduled = sys.argv[-1] == "--scheduled"
strategy, trading, certificates, protoc, proto_root = sys.argv[1:-1] if scheduled else sys.argv[1:]
strategy=os.environ.get("ASTERION_STRATEGY_EXECUTABLE",strategy)
trading=os.environ.get("ASTERION_TRADING_EXECUTABLE",trading)
for binary in (strategy,trading,certificates,protoc):
    if not Path(binary).is_file(): raise SystemExit(f"Missing test executable: {binary}")

def wire(schema, kind, content, decode=False):
    package = "protocol" if schema == "trading" else schema
    output = subprocess.run([protoc, f"--proto_path={proto_root}",
        f"--{'decode' if decode else 'encode'}=asterion.{package}.v1.{kind}",
        str(Path(proto_root)/f"asterion/v1/{schema}.proto")], input=content,
        capture_output=True, check=True).stdout
    # protoc writes text format with CRLF on Windows; assertions use LF.
    return output.replace(b"\r\n", b"\n") if decode else output

def request(schema, operation):
    prefix = "version: 1 correlation_id: 'request' session_id: "
    prefix += "'account' mode: PAPER " if schema == "trading" else "'automatic' "
    return wire(schema, "Request", (prefix+operation).encode())

def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

with tempfile.TemporaryDirectory(prefix="asterion-auto-replay-", ignore_cleanup_errors=True) as folder:
    root=Path(folder)
    for name in ("strategy", "account"): (root/name).mkdir()
    subprocess.run([certificates, folder], check=True, capture_output=True)
    context=ssl.create_default_context(cafile=str(root/"ca.crt"))
    context.load_cert_chain(str(root/"client.crt"), str(root/"client.key"))
    trade_port, strategy_port=port(), port()
    while trade_port==strategy_port: strategy_port=port()
    def call(schema, operation):
        payload=request(schema, operation)
        target=trade_port if schema=="trading" else strategy_port
        with socket.create_connection(("127.0.0.1", target), timeout=5) as sock, context.wrap_socket(sock, server_hostname="localhost") as channel:
            channel.sendall(struct.pack("!I",len(payload))+payload)
            def read(count):
                result=b""
                while len(result)<count:
                    chunk=channel.recv(count-len(result))
                    assert chunk, "service closed reply"
                    result+=chunk
                return result
            size=struct.unpack("!I",read(4))[0]
            assert 0<size<=16*1024*1024
            result=wire(schema,"Response",read(size),True)
            assert b"error {" not in result, result
            return result
    def start(binary, session, directory, target, mode=False):
        args=[binary,"--session",session,"--directory",str(directory),"--bind","127.0.0.1","--port",str(target),
              "--tls-ca",str(root/"ca.crt"),"--tls-cert",str(root/"server.crt"),"--tls-key",str(root/"server.key")]
        if mode: args += ["--mode","paper"]
        process=subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            assert process.poll() is None, process.stderr.read()
            try:
                call("trading" if mode else "strategy","heartbeat {}")
                return process
            except OSError: time.sleep(.05)
        process.kill();process.communicate(timeout=10)
        raise AssertionError("service startup timed out")
    def stop(process):
        if process.poll() is None: process.kill()
        process.communicate(timeout=10)
    account=start(trading,"account",root/"account",trade_port,True)
    host=None
    proxy=None
    thread=None
    proxy_stop=threading.Event()
    settlement_committed=threading.Event()
    release_reply=threading.Event()
    proxy_errors=[]
    replay_port=trade_port
    if scheduled:
        server_context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server_context.load_cert_chain(str(root/"server.crt"),str(root/"server.key"))
        server_context.load_verify_locations(str(root/"ca.crt"))
        server_context.verify_mode=ssl.CERT_REQUIRED
        proxy=socket.socket();proxy.bind(("127.0.0.1",0));proxy.listen();proxy.settimeout(.2)
        replay_port=proxy.getsockname()[1]
        def read_frame(channel):
            def read(count):
                value=b""
                while len(value)<count:
                    chunk=channel.recv(count-len(value))
                    if not chunk: raise ConnectionError("closed proxy frame")
                    value+=chunk
                return value
            header=read(4);size=struct.unpack("!I",header)[0]
            assert 0<size<=16*1024*1024
            return header+read(size)
        def forward():
            while not proxy_stop.is_set():
                try:
                    sock,_=proxy.accept();sock.settimeout(5)
                    with server_context.wrap_socket(sock,server_side=True) as downstream:
                        request_frame=read_frame(downstream)
                        decoded=wire("trading","Request",request_frame[4:],True)
                        with socket.create_connection(("127.0.0.1",trade_port),timeout=5) as upstream_socket, context.wrap_socket(upstream_socket,server_hostname="localhost") as upstream:
                            upstream.sendall(request_frame);reply=read_frame(upstream)
                        if b"replay_settle {" in decoded and not settlement_committed.is_set():
                            assert b"error {" not in wire("trading","Response",reply[4:],True)
                            settlement_committed.set()
                            release_reply.wait(15)
                            continue
                        downstream.sendall(reply)
                except (OSError,ssl.SSLError):
                    continue
                except Exception as error:
                    proxy_errors.append(repr(error));return
        thread=threading.Thread(target=forward,daemon=True);thread.start()
    try:
        contract={"venue":"SHFE","symbol":"rb2610","currency":"CNY","price_increment":"1","quantity_increment":"1","multiplier":"10","product":"rb","delivery_month":"2026-10"}
        bars=[{"trading_day":"2026-09-25" if i<3 or not scheduled else "2026-09-28","timestamp_ns":str(i+1),**{key:str(price) for key in ("open","high","low","close")},"volume":"10"} for i,price in enumerate([100,101,100,102,99,103])]
        days=[{"trading_day":"2026-09-25","settlement_price":"105"},{"trading_day":"2026-09-28","settlement_price":"110"}] if scheduled else [{"trading_day":"2026-09-25","settlement_price":"110"}]
        dataset={"version":1,"contract":contract,"interval_minutes":1,"bars":bars,"days":days}
        revision=hashlib.sha256(json.dumps(dataset,sort_keys=True,separators=(",",":")).encode()).hexdigest()
        spec='''contract { venue: "SHFE" symbol: "rb2610" currency: "CNY" price_increment { units: 100000000 } quantity_increment { units: 100000000 } multiplier { units: 1000000000 } product: "rb" delivery_month: "2026-10" }'''
        events=" ".join('bars { trading_day: "'+b["trading_day"]+'" timestamp_ns: '+b["timestamp_ns"]+' '+" ".join(f'{key} {{ units: {int(b[key])*100000000} }}' for key in ("open","high","low","close","volume"))+' }' for b in bars)
        day_wire=" ".join(f'days {{ trading_day: "{d["trading_day"]}" settlement_price {{ units: {int(d["settlement_price"])*100000000} }} }}' for d in days)
        dataset_wire=f'dataset {{ version: 1 revision: "{revision}" {spec} interval_minutes: 1 {events} {day_wire} source: "test.fixture" source_task_id: "test-bars" settlement_task_id: "test-settlement" manifest_sha256: "{"a"*64}" settlement_manifest_sha256: "{"b"*64}" }}'
        call("trading",f'''create {{ risk {{ max_order_quantity {{ units: 10000000000 }} max_gross_quantity {{ units: 10000000000 }} max_working_orders: 100 }} deposit {{ units: 100000000000 }} contracts {{ {dataset_wire} cost_schedule {{ versions {{ effective_from: "1970-01-01" source: "test fixture" values {{ margin_per_lot {{ units: 10000000000 }} open_fee {{ units: 200000000 }} close_today_fee {{ units: 300000000 }} close_yesterday_fee {{ units: 400000000 }} margin_rate {{ units: 0 }} open_fee_rate {{ units: 0 }} close_today_fee_rate {{ units: 0 }} close_yesterday_fee_rate {{ units: 0 }} }} }} }} }} }}''')
        call("trading",f'''command {{ request_id: "grant.request" strategy_grant {{ grant_id: "grant" strategy_id: "automatic" stream_id: "history" dataset_revision: "{revision}" max_quantity {{ units: 100000000 }} }} }}''')
        host=start(strategy,"automatic",root/"strategy",strategy_port)
        plan_dataset=dataset_wire.replace("dataset {","datasets {",1)
        plan=f'''replay {{ version: 4 {plan_dataset} trading_session: "account" grant_id: "grant" host: "localhost" port: {replay_port} tls_ca: {json.dumps(str(root/"ca.crt"))} tls_cert: {json.dumps(str(root/"client.crt"))} tls_key: {json.dumps(str(root/"client.key"))} }}'''
        call("strategy",f'''create {{ version: 2 session_id: "automatic" stream_id: "history" {spec.replace("contract {","contracts {",1)} plugin_id: "asterion.strategy.cta.sma-long-flat" fast: 1 slow: 2 quantity {{ units: 100000000 }} {plan} }}''')
        def complete():
            deadline=time.monotonic()+20
            while time.monotonic()<deadline:
                status=call("strategy","snapshot {}")
                assert b'phase: "blocked"' not in status,status
                if b'phase: "completed"' in status: return
                time.sleep(.1)
            raise AssertionError("automatic replay did not finish")
        if scheduled:
            assert settlement_committed.wait(20),proxy_errors
            stop(host);host=None
            stop(account)
            release_reply.set()
            account=start(trading,"account",root/"account",trade_port,True)
            boundary=call("trading","snapshot {}")
            assert b"settled_days: 1" in boundary and b"cursor: 3" in boundary,boundary
            host=start(strategy,"automatic",root/"strategy",strategy_port)
        complete()
        final=call("trading","snapshot {}")
        assert (b"balance {\n    units: 102400000000\n  }" if scheduled else b"balance {\n    units: 105000000000\n  }") in final, final
        assert (b"fees {\n    units: 600000000\n  }" if scheduled else b"fees {\n    units: 1000000000\n  }") in final, final
        assert final.count(b"  fills {")==(2 if scheduled else 4),final
        assert b"active: true" not in final and b"  positions {" not in final, final
        stop(host);host=start(strategy,"automatic",root/"strategy",strategy_port)
        complete()
        assert call("trading","snapshot {}")==final
    finally:
        proxy_stop.set();release_reply.set()
        if proxy: proxy.close()
        if thread: thread.join(timeout=6)
        if host: stop(host)
        stop(account)
if scheduled:
    assert not proxy_errors,proxy_errors
    assert b"settled_days: 2" in final,final
    print("Scheduled settlement committed with reply withheld; both processes killed and recovered without duplicate ledger effects")
print("Automatic strategy replay over mTLS, completion revocation and restart idempotency verified")
