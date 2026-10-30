"""Native market client and service over plain TCP on the local network, with a test-only market SDK."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
build=Path(sys.argv[1]).resolve()
sdk='libasterion_test_ctp.dylib' if sys.platform=='darwin' else 'libasterion_test_ctp.so'
with tempfile.TemporaryDirectory(prefix='asterion-market-tcp-', ignore_cleanup_errors=True) as folder:
    with socket.socket() as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    server=subprocess.Popen([str(build/('asterion_market')),'--session','market.test','--directory',folder,'--bind','127.0.0.1','--port',str(port),'--ctp-library',str(build/sdk)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            try:
                with socket.create_connection(('127.0.0.1',port),timeout=.1):break
            except OSError:time.sleep(.02)
        # A connection that does not start with the magic gets no application data.
        with socket.create_connection(('127.0.0.1',port),timeout=2) as channel:
            channel.sendall(b'HTTP\x00\x00\x00\x01x')
            try:
                assert channel.recv(1)==b'', 'a client without the magic received application data'
            except ConnectionResetError:
                pass
        subprocess.run([str(build/('asterion_test_market_remote')),str(port)],check=True,timeout=15)
        assert server.poll() is None, 'Terminal disconnect must not stop the service'
    finally:
        server.terminate();server.communicate(timeout=8)
print('Market closes connections without the magic; the native client receives quotes and disconnects independently')
