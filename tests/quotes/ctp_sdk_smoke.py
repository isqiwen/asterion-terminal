"""Check both vendor ABIs and loopback lifecycles using test-only credentials."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
build = Path(sys.argv[1]).resolve()
if sys.platform != 'darwin' and not sys.platform.startswith('linux'):
    raise SystemExit('Vendor SDK acceptance requires macOS or Linux')
suffix = '.dylib' if sys.platform == 'darwin' else '.so'
library = Path(os.environ.get('ASTERION_CTP_LIBRARY', str(build / ('ctp-md' + suffix))))
trader_library = Path(os.environ.get('ASTERION_CTP_CATALOG_LIBRARY', str(build / ('ctp-trader' + suffix))))
for sdk_path in (library, trader_library):
    if not sdk_path.is_file():
        raise SystemExit('Required vendor SDK is missing: ' + str(sdk_path))
import ctypes
sdk = ctypes.CDLL(str(library))
name = '_ZN15CThostFtdcMdApi13GetApiVersionEv'
version = getattr(sdk, name);version.restype = ctypes.c_char_p
assert b'6.7.7' in version(), version()
print('Packaged CTP vendor SDK loaded, version:', version().decode())
sys.path.insert(0, str(Path(__file__).parents[1] / "support"))
from history_installation import quotes_host
with tempfile.TemporaryDirectory(prefix="asterion-vendor-market-") as folder:
    with quotes_host(build, Path(folder), vendor=True):
        time.sleep(.5)
subprocess.run([str(build / 'asterion_ctp_trader_tests'),
                '--gtest_filter=CtpVendor.TraderLoopbackLifecycleUsesThePackagedAbi'],
               env=dict(os.environ, ASTERION_VENDOR_CTP_TRADER_LIBRARY=str(trader_library)),
               check=True, timeout=20)
print('Vendor market and trader factories, loopback startup and Release passed; no external server contacted')
