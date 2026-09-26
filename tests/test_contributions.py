import pytest

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
