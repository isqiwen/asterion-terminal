"""Development-only strategy package for installer mechanism tests."""

import io
import json
import zipfile

DEFAULT_SCRIPT = """from asterion_plugin_sdk import serve_strategy


class Flat:
    def __init__(self, parameters):
        pass

    def on_close(self, trading_day, close):
        return False


serve_strategy(Flat, lambda parameters: 1)
"""


def package_content(*, identifier="test.strategy", version="1.0.0", script=None, extra=None):
    manifest = {
        "api_version": 1,
        "id": identifier,
        "version": version,
        "title": "测试策略",
        "description": "仅用于开发测试",
        "layer": "L3",
        "runtime": "python",
        "entry": "plugin.py",
        "trust": "local-code",
        "contributions": {
            "research.strategy": {
                "id": identifier,
                "name": "测试策略",
                "description": "始终空仓",
                "data_type": "futures.daily",
                "execution": "long-flat-next-open",
                "dependencies": "stdlib-and-sdk",
                "parameters": [],
            }
        },
    }
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as archive:
        archive.writestr("manifest.json", json.dumps(manifest))
        archive.writestr("plugin.py", DEFAULT_SCRIPT if script is None else script)
        for name, content in (extra or {}).items():
            archive.writestr(name, content)
    return stream.getvalue()
