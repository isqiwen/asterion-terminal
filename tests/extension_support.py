"""Development-only external provider, shipped nowhere in production registration."""

import io
import json
import zipfile

from asterion.data.providers.public import Capability, ProviderManifest


def package_content(
    *, identifier="test.calendar", version="1.0.0", script=None, extra=None, requires=None
):
    provider = ProviderManifest(
        id=identifier.replace(".", "_"),
        name="测试日历",
        version=version,
        capabilities=[
            Capability(
                id="calendar",
                label="日历",
                type_id="futures.calendar",
                exchanges=["SHFE"],
                date_range=True,
                description="Test fixture",
            )
        ],
    )
    manifest = {
        "api_version": 1,
        "id": identifier,
        "version": version,
        "title": "测试日历",
        "description": "仅用于开发测试",
        "runtime": "python",
        "entry": "plugin.py",
        "trust": "local-code",
        "requires": requires or {},
        "contributions": {"data.provider": provider.model_dump(mode="json")},
    }
    if script is None:
        script = """from asterion_plugin_sdk import serve

def dispatch(method, params):
    if method == "plan":
        return [{"api": "calendar", "params": {}, "fields": ["exchange", "date", "is_open", "previous_trading_day"], "limit": 100, "start": params["request"]["start"], "end": params["request"]["end"]}]
    if method == "probe":
        return "ok"
    if method == "fetch":
        return [{"exchange": "SHFE", "date": "2024-01-02", "is_open": 1, "previous_trading_day": "2023-12-29"}]
    if method == "normalize":
        return params["rows"]
    raise ValueError("unknown method")

serve(dispatch)
"""
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as archive:
        archive.writestr("manifest.json", json.dumps(manifest))
        archive.writestr("plugin.py", script)
        for name, content in (extra or {}).items():
            archive.writestr(name, content)
    return stream.getvalue()
