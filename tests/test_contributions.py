import pytest
from synthetic_provider import Synthetic

from asterion.data.providers import ProviderRegistry, builtin_registry
from asterion.platform.registry import Registry


def test_registration_is_atomic_and_discovery_does_not_invoke_contributions():
    def fixture():
        pytest.fail("Discovery must not execute a contribution")

    registry = Registry((("fixture.report", fixture),))
    with pytest.raises(ValueError, match="Duplicate contribution"):
        registry.register("fixture.report", object())
    with pytest.raises(ValueError, match="Contribution ID"):
        registry.register(" ", object())
    with pytest.raises(KeyError):
        registry.get("fixture.missing")
    assert registry.all() == (fixture,)
    assert registry.get("fixture.report") is fixture


def test_test_only_provider_uses_same_registration_without_entering_production():
    provider = Synthetic()
    registry = ProviderRegistry((provider,))
    assert registry.get(provider.manifest.id).manifest == provider.manifest
    with pytest.raises(ValueError, match="Duplicate contribution"):
        registry.register(provider)
    provider.manifest = provider.manifest.model_copy(update={"api_version": -1})
    with pytest.raises(ValueError, match="Unsupported provider contract"):
        ProviderRegistry((provider,))
    assert [item.manifest.id for item in builtin_registry().all()] == ["tushare"]
