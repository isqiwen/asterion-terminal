"""Native market client/server over real mutual TLS with a test-only market SDK."""
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import time
build=Path(sys.argv[1]).resolve()
sdk='libasterion_test_ctp.dylib' if sys.platform=='darwin' else 'libasterion_test_ctp.so'
with tempfile.TemporaryDirectory(prefix='asterion-market-tls-', ignore_cleanup_errors=True) as folder:
    root=Path(folder)
    subprocess.run([str(build/('asterion_test_certificates')),folder],check=True)
    with socket.socket() as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    server=subprocess.Popen([str(build/('asterion-market-data')),'--session','market.test','--directory',folder,'--bind','127.0.0.1','--port',str(port),'--tls-ca',str(root/'ca.crt'),'--tls-cert',str(root/'server.crt'),'--tls-key',str(root/'server.key'),'--ctp-library',str(build/sdk)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            try:
                with socket.create_connection(('127.0.0.1',port),timeout=.1):break
            except OSError:time.sleep(.02)
        context=ssl.create_default_context(cafile=str(root/'ca.crt'))
        try:
            with socket.create_connection(('127.0.0.1',port),timeout=2) as sock, context.wrap_socket(sock,server_hostname='localhost') as channel:
                channel.sendall(b'\x00\x00\x00\x01x')
                assert channel.recv(1)==b'', 'Unauthenticated client received application data'
        except ssl.SSLError:pass
        subprocess.run([str(build/('asterion_test_market_remote')),str(port),folder],check=True,timeout=15)
        assert server.poll() is None, 'Terminal disconnect must not stop the service'
    finally:
        server.terminate();server.communicate(timeout=8)
print('Market mTLS rejects anonymous clients; authenticated native client receives quotes and disconnects independently')
