"""Cross-language ownership, immutable indexes and generated domain contracts."""

from datetime import UTC, date, datetime
from decimal import Decimal, localcontext

import pytest
from asterion_bindings import _native
from asterion_bindings.calendar import TimeSpec, time_id
from asterion_bindings.catalog import (
    ImportIdentity,
    ReferenceCatalog,
    SourceIdentity,
    catalog_digest,
)
from asterion_bindings.rules import RulePeriod
from import_identity_support import import_identity, source_identity
from pydantic import ValidationError
from test_reference import fixture
from test_trading_time import example


def test_immutable_dtos_cannot_diverge_from_their_native_indexes():
    catalog = ReferenceCatalog.model_validate(fixture())
    original = catalog_digest(catalog)
    with pytest.raises(ValidationError):
        catalog.schema_version = 8
    with pytest.raises(TypeError):
        catalog.contracts[0] = catalog.contracts[0]
    copy = catalog.model_copy(deep=True)
    assert copy == catalog
    assert catalog_digest(copy) == original
    assert copy._native_handle is not catalog._native_handle


def test_calendar_copy_rebuilds_the_index_and_rejects_nonpositive_bars():
    spec = TimeSpec.model_validate(example())
    changed = spec.model_copy(update={"title": "独立规则版本"})
    assert time_id(changed) != time_id(spec)
    day = date(2025, 4, 14)
    span = spec.daily("SHFE.au2506", day)[0]
    for seconds in (0, -60):
        with pytest.raises(ValueError):
            spec.validate_bar("SHFE.au2506", span.start, day, seconds, "bar_start")


def test_native_handles_refuse_wrong_operations_and_unknown_arguments():
    spec = TimeSpec.model_validate(example())
    with pytest.raises(ValueError):
        spec._native_handle.call("daily", '{"contract":"SHFE.au2506","day":"2025-04-14","extra":1}')
    with pytest.raises(ValueError):
        spec._native_handle.call("authorize", "{}")
    with pytest.raises(ValueError):
        _native.DomainHandle("kernel", "Plugin", "{}")


def test_rule_fee_preserves_the_explicit_research_decimal_context():
    period = RulePeriod(
        start=date(2025, 1, 1),
        end=date(2025, 1, 2),
        settlement_basis=None,
        margin_rate=Decimal("0.1"),
        fee_mode="notional",
        open_fee=Decimal("0.12345678"),
        close_fee=Decimal("0.12345678"),
    )
    price = Decimal("12345678901234567890.12345678")
    multiplier = Decimal("123.12345678")
    for precision in (28, 40):
        with localcontext() as context:
            context.prec = precision
            expected = period.open_fee * 3 * (price * multiplier)
            assert period.fee(price, multiplier, 3, True) == expected


@pytest.mark.parametrize("day", [date(2024, 1, 2), "2024-01-02"])
def test_identity_validation_projects_arrow_columns_without_mutating_rows(day):
    imported = ImportIdentity.model_validate(import_identity())
    sourced = SourceIdentity.model_validate(source_identity("SHFE.rb2405", "fixture", "RB2405"))
    row = {
        "contract": "SHFE.rb2405",
        "trading_day": day,
        "symbol": "RB2405",
        "exchange": "SHFE",
        "timestamp": datetime(2024, 1, 2, 1, tzinfo=UTC),
        "open": Decimal("10.25"),
    }
    before = dict(row)
    imported.validate_rows([row])
    assert sourced.validate_rows([row], "SHFE") == ["SHFE.RB.202405.20230516"]
    assert row == before


@pytest.mark.parametrize("day", [datetime(2024, 1, 2, tzinfo=UTC), None, 20240102, "2023-01-02"])
def test_identity_projection_cannot_coerce_invalid_days_or_skip_lifecycle(day):
    imported = ImportIdentity.model_validate(import_identity())
    sourced = SourceIdentity.model_validate(source_identity("SHFE.rb2405", "fixture", "RB2405"))
    row = {"contract": "SHFE.rb2405", "trading_day": day, "symbol": "RB2405", "exchange": "SHFE"}
    with pytest.raises(ValueError):
        imported.validate_rows([row])
    with pytest.raises(ValueError):
        sourced.validate_rows([row], "SHFE")
