import pytest


@pytest.fixture
def development_providers(monkeypatch):
    """Opt-in fixture: synthetic code never enters the production registry/package."""
    from synthetic_provider import Synthetic

    from asterion.data.providers import ProviderRegistry
    from asterion.data.providers.tushare import Tushare
    from asterion.data.types import builtin_types
    from asterion.data.types.public import DataType

    def test_types():
        types = builtin_types()
        daily = types.get("futures.daily")
        types.register(
            DataType(
                daily.manifest.model_copy(update={"id": "testing.synthetic"}),
                daily.validate,
                chart=daily.chart,
            )
        )
        return types

    monkeypatch.setattr("asterion.data.library.builtin_types", test_types)
    monkeypatch.setattr(
        "asterion.data.sync.builtin_registry",
        lambda root=None: ProviderRegistry((Tushare(), Synthetic())),
    )


@pytest.fixture
def identity_instances(monkeypatch):
    """Capture test-owned identity services without exporting them through the HTTP host."""
    from asterion.identity.service import Identity

    instances = []

    def create(*args, **kwargs):
        instance = Identity(*args, **kwargs)
        instances.append(instance)
        return instance

    monkeypatch.setattr("asterion.identity.plugin.Identity", create)
    return instances


@pytest.fixture
def system_postgres():
    """Use the current platform package layout; each test owns a separate database cluster."""
    import platform
    import sys
    from pathlib import Path

    from asterion.runtime.desktop import pg_directory

    root = (
        Path("/usr")
        if sys.platform == "linux"
        else Path("/opt/homebrew" if platform.machine() == "arm64" else "/usr/local")
        / "opt/postgresql@17"
    )
    if not (pg_directory(root, "bindir") / "postgres").is_file():
        pytest.skip("system PostgreSQL 17 required")
    return root
