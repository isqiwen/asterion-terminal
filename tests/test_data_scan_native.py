"""Real Arrow transfer and delayed native ownership checks for fixed scans."""

import hashlib
import json
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from asterion_bindings import _native
from asterion_bindings.data_scan import ScanBatches, open_scan
from asterion_bindings.data_store import ScanRequest
from asterion_bindings.database import create_engine
from asterion_bindings.plugin_host import Activation, Plugin, PluginHost
from asterion_bindings.resource import Resource
from asterion_bindings.storage import Storage
from asterion_bindings.tasks import ExecutionContext
from fastapi import FastAPI
from import_identity_support import import_identity


@pytest.fixture
def source(tmp_path):
    engine = create_engine("sqlite://")
    store = Storage(engine, ())
    path = tmp_path / "fixed.parquet"
    table = pa.table(
        {
            "contract": ["SHFE.rb2405", "SHFE.rb2405"],
            "trading_day": ["2024-01-02", "2024-01-03"],
            "close": ["3200.00100", "3201.00100"],
        }
    )
    pq.write_table(table, path)
    identity = import_identity()
    actual = identity["catalog"]["contracts"][0]["id"]
    version = {
        "id": "fixed",
        "rows": 2,
        "manifest": {
            "layer": "STANDARD",
            "type": {
                "id": "futures.daily",
                "fields": [{"name": name} for name in table.schema.names],
            },
            "scope": {"contract_ids": [actual]},
            "source": "local_file",
            "format": "parquet",
            "path": path.name,
            "checksum": hashlib.sha256(path.read_bytes()).hexdigest(),
            "import_options": {"identity": identity},
        },
    }
    request = ScanRequest(
        version_id="fixed",
        contract_ids=(actual,),
        columns=("close",),
        start="2024-01-02",
        end="2024-01-03",
        batch_rows=1,
    )

    class Source:
        def scan(self):
            return open_scan(tmp_path, store._scope, version, request)

    value = Source()
    value.store = store
    value.root = tmp_path
    value.version = version
    value.request = request
    try:
        yield value
    finally:
        store.close()
        engine.dispose()


def test_arrow_batches_keep_exact_values_and_remain_owned_after_scanner_close(source):
    native = source.scan()
    batches = ScanBatches(native)
    first = next(batches)
    assert isinstance(first, pa.RecordBatch)
    assert first.to_pylist() == [
        {
            "close": "3200.00100",
            "_contract_id": source.request.contract_ids[0],
            "_observed_at": None,
            "_raw_version_id": None,
        }
    ]
    batches.close()
    assert native.metrics()["closed"] is True
    assert first.column(0).to_pylist() == ["3200.00100"]
    with pytest.raises(StopIteration):
        next(batches)


def test_close_before_first_batch_releases_root_descriptor(source):
    native = source.scan()
    batches = ScanBatches(native)
    assert native.metrics()["files_opened"] == 0
    batches.close()
    assert native.metrics()["closed"] is True
    fd_root = Path("/proc/self/fd")
    if fd_root.exists():
        targets = []
        for entry in fd_root.iterdir():
            try:
                targets.append(entry.readlink())
            except FileNotFoundError:
                pass
        assert not any(target.is_relative_to(source.root) for target in targets)


def test_storage_close_revokes_delayed_read_and_closes_active_file(source):
    native = source.scan()
    assert native.next_batch().num_rows == 1
    source.store.close()
    with pytest.raises(ValueError, match="closed"):
        native.next_batch()
    assert native.metrics()["closed"] is True
    native.close()


def test_host_lifetime_is_captured_before_lazy_read(source):
    resource = Resource("fixture.scan", type(source))
    retained = []

    def activate(context):
        retained.append(context.resource(resource))
        return Activation()

    host = PluginHost((Plugin("fixture.owner", (), activate, resources=(resource,)),))
    host.activate(FastAPI(), {"fixture.owner": {resource: source}})
    try:
        native = retained[0].scan()
        host.close()
        with pytest.raises(ValueError, match="closed|not active"):
            native.next_batch()
        assert native.metrics() == {
            "files_opened": 0,
            "row_groups_decoded": 0,
            "rows_decoded": 0,
            "verified_bytes": 0,
            "closed": True,
        }
    finally:
        host.close()


def test_task_execution_close_revokes_retained_scan_after_first_batch(source):
    resource = Resource("fixture.scan", type(source))
    execution = ExecutionContext((resource,), {resource: source})
    try:
        native = execution.resource(resource).scan()
        assert native.next_batch().num_rows == 1
        execution.close()
        with pytest.raises(ValueError, match="closed|active"):
            native.next_batch()
        assert native.metrics()["closed"] is True
    finally:
        execution.close()


def test_lifetime_cannot_be_replaced_with_arbitrary_python_callback(source):
    class Fake:
        def check(self):
            pytest.fail("Native ownership must not call an arbitrary Python guard")

    files = _native.ReadFilesHandle(source.root, 1024**3, 100)
    with pytest.raises(ValueError, match="native resource lifetime"):
        _native.NativeDataScan(
            files,
            source.store._scope,
            [Fake()],
            json.dumps(source.version),
            source.request.model_dump_json(),
        )


@pytest.mark.parametrize("action", ["close", "revoke", "conversion_error"])
def test_arrow_conversion_reentrancy_never_delivers_revoked_batch(source, action):
    # arrow-pyarrow caches the imported reader type once per process. A fresh
    # interpreter exercises the real C Data conversion with a reentrant import.
    import subprocess
    import sys
    import textwrap

    script = textwrap.dedent(
        """
        import json
        import sys
        from pathlib import Path
        import pyarrow as pa
        from asterion_bindings.data_scan import open_scan
        from asterion_bindings.data_store import ScanRequest
        from asterion_bindings.database import create_engine
        from asterion_bindings.storage import Storage

        engine = create_engine("sqlite://")
        store = Storage(engine, ())
        real_reader = pa.RecordBatchReader
        action = sys.argv[4]
        class ReentrantReader:
            @staticmethod
            def _import_from_c(address):
                reader = real_reader._import_from_c(address)
                if action == "close":
                    native.close()
                elif action == "revoke":
                    store.close()
                else:
                    raise RuntimeError("conversion failed")
                return reader
        pa.RecordBatchReader = ReentrantReader
        native = open_scan(
            Path(sys.argv[1]), store._scope, json.loads(sys.argv[2]),
            ScanRequest.model_validate_json(sys.argv[3]),
        )
        try:
            native.next_batch()
        except (ValueError, RuntimeError) as error:
            assert any(text in str(error) for text in ("关闭", "closed", "conversion failed"))
        else:
            raise AssertionError("revoked batch was delivered")
        assert native.metrics()["closed"] is True
        native.close()
        store.close()
        engine.dispose()
        """
    )
    result = subprocess.run(
        [
            sys.executable,
            "-c",
            script,
            str(source.root),
            json.dumps(source.version),
            source.request.model_dump_json(),
            action,
        ],
        capture_output=True,
        text=True,
        timeout=15,
        check=False,
    )
    assert result.returncode == 0, result.stderr
