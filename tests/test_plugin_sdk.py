import hashlib
import json
import subprocess
import sys
from pathlib import Path

import pytest

from asterion_plugin_sdk.packages import PackageManifest, checked_archive

EXAMPLES = Path(__file__).resolve().parents[1] / "examples" / "plugins"


def pack(source, output):
    return subprocess.run(
        [sys.executable, "-m", "asterion_plugin_sdk", "pack", str(source), str(output)],
        capture_output=True,
        text=True,
        check=False,
    )


def test_public_sdk_packages_the_strategy_example_deterministically(tmp_path):
    example = "close-momentum"
    outputs = [tmp_path / f"package-{index}.zip" for index in range(2)]
    for output in outputs:
        result = pack(EXAMPLES / example, output)
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == hashlib.sha256(output.read_bytes()).hexdigest()
    assert outputs[0].read_bytes() == outputs[1].read_bytes()
    manifest, _ = checked_archive(outputs[0].read_bytes())
    assert manifest.layer == "L3"
    assert len(manifest.contributions) == 1


@pytest.mark.parametrize(
    "mutation",
    ["missing_layer", "L0", "L1", "L2", "L4", "L5", "data_provider", "empty", "unknown"],
)
def test_sdk_rejects_invalid_current_manifest_before_creating_output(tmp_path, mutation):
    current = PackageManifest.model_validate_json(
        (EXAMPLES / "close-momentum" / "manifest.json").read_bytes()
    ).model_dump()
    if mutation == "missing_layer":
        current.pop("layer")
    elif mutation == "data_provider":
        # Data sources are built-in Rust implementations only.
        current["contributions"] = {"data.provider": {}}
    elif mutation == "empty":
        current["contributions"] = {}
    elif mutation == "unknown":
        current["contributions"] = {"unknown.extension": {}}
    else:
        current["layer"] = mutation
    source = tmp_path / "source"
    source.mkdir()
    (source / "manifest.json").write_text(json.dumps(current))
    (source / "plugin.py").write_text('raise RuntimeError("must not execute during packing")\n')
    output = tmp_path / "package.zip"
    result = pack(source, output)
    assert result.returncode != 0
    assert "Invalid plugin package" in result.stderr
    assert not output.exists()


@pytest.mark.parametrize("defect", ["missing_entry", "native_file", "invalid_path"])
def test_sdk_uses_shared_archive_admission_before_writing(tmp_path, defect):
    source = tmp_path / "source"
    source.mkdir()
    (source / "manifest.json").write_bytes(
        (EXAMPLES / "close-momentum" / "manifest.json").read_bytes()
    )
    if defect != "missing_entry":
        (source / "plugin.py").write_text("pass\n")
    if defect == "native_file":
        (source / "extension.so").write_bytes(b"fixture")
    if defect == "invalid_path":
        (source / "invalid name.txt").write_text("fixture")
    output = tmp_path / "package.zip"
    result = pack(source, output)
    assert result.returncode != 0
    assert "Invalid plugin package" in result.stderr
    assert not output.exists()


def test_public_package_contract_does_not_import_terminal_implementation():
    subprocess.run(
        [
            sys.executable,
            "-c",
            (
                "import sys; from asterion_plugin_sdk.packages import PackageManifest; "
                "assert not any(name == 'asterion' or name.startswith('asterion.') "
                "for name in sys.modules)"
            ),
        ],
        check=True,
        capture_output=True,
    )
