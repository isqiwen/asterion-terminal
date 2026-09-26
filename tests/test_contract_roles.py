"""Explicit offline role observations, not exchange assertions or shipped product defaults."""

from copy import deepcopy
from datetime import datetime

import pytest
from asterion_bindings.calendar import TimeSpec, TimeVersion, time_id
from asterion_bindings.database import create_engine
from asterion_bindings.roles import (
    NextOpening,
    RoleQuery,
    RoleSpec,
    RoleVersion,
    next_opening,
    resolve,
    role_id,
)
from fastapi.testclient import TestClient
from import_identity_support import import_identity
from test_trading_time import example

from asterion.api.app import create_app
from asterion.contract_roles.plugin import RoleBackup, validate
from asterion.platform.config import Settings


def fixture():
    time = TimeSpec.model_validate(example())
    catalog = import_identity("SHFE.au2506")["catalog"]
    actual = catalog["contracts"][0]
    spec = RoleSpec.model_validate(
        {
            "schema_version": 1,
            "origin": "provider_report",
            "source": "offline-fixture",
            "source_version": "report-1",
            "source_checksum": "a" * 64,
            "observed_at": "2025-04-15T00:00:00Z",
            "product_id": "SHFE.AU",
            "catalog": catalog,
            "trading_time": TimeVersion(id=time_id(time), spec=time).model_dump(mode="json"),
            "reports": [
                {
                    "trading_day": "2025-04-14",
                    "role": "main",
                    "contract_id": actual["id"],
                    "available_at": "2025-04-11T20:00:00+08:00",
                    "evidence": "Explicit offline report",
                }
            ],
        }
    )
    return RoleVersion(id=role_id(spec), spec=spec)


def query(version, **values):
    return RoleQuery.model_validate(
        {
            "version_id": version.id,
            "role": "main",
            "timestamp": "2025-04-11T21:00:00+08:00",
            "information_at": "2025-04-11T21:00:00+08:00",
            "mode": "as_known",
            "explanation": "",
        }
        | values
    )


def test_weekend_night_resolves_monday_role_and_freezes_actual():
    version = fixture()
    result = resolve(version, query(version))
    assert str(result.report.trading_day) == "2025-04-14"
    assert result.contract.id == "SHFE.AU.202506.20240617"
    assert result.session.session == "night"
    assert result.execution_authorized is False


@pytest.mark.parametrize(
    "change",
    [
        "duplicate",
        "cross_product",
        "outside_lifecycle",
        "closed",
        "future_observation",
        "same_secondary",
        "algorithm",
    ],
)
def test_invalid_reports_are_rejected(change):
    value = fixture().spec.model_dump(mode="json")
    row = value["reports"][0]
    if change == "duplicate":
        value["reports"].append(deepcopy(row))
    if change == "cross_product":
        value["product_id"] = "SHFE.RB"
    if change == "outside_lifecycle":
        row["trading_day"] = "2026-04-14"
    if change == "closed":
        row["trading_day"] = "2025-04-13"
    if change == "future_observation":
        row["available_at"] = "2026-04-14T00:00:00Z"
    if change == "same_secondary":
        value["reports"].append(row | {"role": "secondary"})
    if change == "algorithm":
        value["origin"] = "computed"
    with pytest.raises(ValueError):
        RoleSpec.model_validate(value)


def test_missing_day_or_role_does_not_forward_fill():
    version = fixture()
    for changes in (
        {"role": "secondary"},
        {"timestamp": "2025-04-10T21:00:00+08:00", "information_at": "2025-04-10T21:00:00+08:00"},
    ):
        with pytest.raises(ValueError, match="缺口"):
            resolve(version, query(version, **changes))


@pytest.mark.parametrize("known", [None, "2025-04-14T15:00:00+08:00"])
def test_unknown_or_later_publication_cannot_enter_as_known(known):
    value = fixture().spec.model_dump(mode="json")
    value["reports"][0]["available_at"] = known
    spec = RoleSpec.model_validate(value)
    version = RoleVersion(id=role_id(spec), spec=spec)
    with pytest.raises(ValueError, match="可知"):
        resolve(version, query(version))
    retrospective = query(
        version,
        mode="retrospective",
        information_at="2025-04-16T00:00:00Z",
        explanation="历史时点可知性未经确认",
    )
    assert resolve(version, retrospective).execution_authorized is False
    with pytest.raises(ValueError):
        query(version, mode="retrospective", explanation="")
    with pytest.raises(ValueError, match="未来信息"):
        query(version, information_at="2025-04-16T00:00:00Z")


def test_contract_information_cutoff_and_mapping_observation_are_checked():
    value = fixture().spec.model_dump(mode="json")
    value["catalog"]["contracts"][0]["provenance"].update(
        observed_at="2026-01-01T00:00:00Z", available_at="2026-01-01T00:00:00Z"
    )
    spec = RoleSpec.model_validate(value)
    version = RoleVersion(id=role_id(spec), spec=spec)
    with pytest.raises(ValueError, match="身份资料"):
        resolve(version, query(version))
    with pytest.raises(ValueError, match="观测"):
        resolve(version, query(version, mode="retrospective", explanation="回溯"))


@pytest.mark.parametrize(
    "end,available,start,day",
    [
        (
            "2025-04-03T15:00:00+08:00",
            "2025-04-03T15:05:00+08:00",
            "2025-04-07T09:00:00+08:00",
            "2025-04-07",
        ),
        (
            "2025-04-11T15:00:00+08:00",
            "2025-04-11T15:05:00+08:00",
            "2025-04-11T21:00:00+08:00",
            "2025-04-14",
        ),
        (
            "2025-04-07T15:00:00+08:00",
            "2025-04-07T21:01:00+08:00",
            "2025-04-08T21:00:00+08:00",
            "2025-04-09",
        ),
    ],
)
def test_next_opening_respects_holiday_night_and_late_arrival(end, available, start, day):
    result = next_opening(
        NextOpening(
            product_id="SHFE.AU",
            trading_time=fixture().spec.trading_time,
            observation_end=end,
            available_at=available,
        )
    )
    assert result.start == datetime.fromisoformat(start)
    assert str(result.trading_day) == day


def test_next_opening_refuses_missing_calendar_and_premature_availability():
    for end, available in [
        ("2025-04-14T15:00:00+08:00", "2025-04-14T16:00:00+08:00"),
        ("2025-04-11T15:00:00+08:00", "2025-04-11T14:59:00+08:00"),
    ]:
        with pytest.raises(ValueError):
            next_opening(
                NextOpening(
                    product_id="SHFE.AU",
                    trading_time=fixture().spec.trading_time,
                    observation_end=end,
                    available_at=available,
                )
            )


def test_api_immutable_persistence_auth_and_backup(tmp_path, monkeypatch):
    engine = create_engine(f"sqlite:///{tmp_path}/roles.db")
    settings = Settings(
        token="role-fixture-token-24-characters", data_root=tmp_path, require_account=False
    )
    from role_source_support import port, sources

    from asterion.contract_roles.sources import RoleSourceRequest, RoleSources

    fake_port = port(sources())
    monkeypatch.setattr(
        RoleSources, "__init__", lambda self, versions: setattr(self, "versions", fake_port)
    )
    spec = RoleSources(fake_port).build(
        RoleSourceRequest(
            mapping_version_id="report-1",
            contracts_version_id="contracts-1",
            trading_time=fixture().spec.trading_time,
        )
    )
    version = RoleVersion(id=role_id(spec), spec=spec)
    try:
        app = create_app(settings, engine)
        with TestClient(app) as client:
            assert client.get("/api/v1/contract-roles").status_code == 401
            client.headers["Authorization"] = "Bearer " + settings.token
            for _ in range(2):
                response = client.post(
                    "/api/v1/contract-roles", json=version.spec.model_dump(mode="json")
                )
                assert response.status_code == 200
                assert response.json() == version.model_dump(mode="json")
            assert len(client.get("/api/v1/contract-roles").json()) == 1
            from asterion.contract_roles.plugin import plugin
            from asterion.contract_roles.public import ROLE_ACCESS
            from asterion.distribution import backup_inputs

            assert app.state.plugins.resolve(ROLE_ACCESS).read(version.id) == version
            (tmp_path / "data").mkdir(exist_ok=True)
            with (
                engine.connect() as conn,
                backup_inputs(conn, tmp_path, settings.token, (plugin,)) as evidence,
            ):
                assert validate(evidence[plugin.id]) == {
                    "contract_role_versions": 1,
                    "computed_role_versions": 0,
                }
            assert (
                client.post(
                    "/api/v1/contract-roles/resolve", json=query(version).model_dump(mode="json")
                ).status_code
                == 200
            )
            assert client.get("/api/v1/contract-roles?limit=0").status_code == 422
        with TestClient(create_app(settings, engine)) as client:
            client.headers["Authorization"] = "Bearer " + settings.token
            assert client.get(f"/api/v1/contract-roles/{version.id}").json() == version.model_dump(
                mode="json"
            )
        record = version.model_dump(mode="json")
        assert validate(RoleBackup((record,), fake_port, (), ())) == {
            "contract_role_versions": 1,
            "computed_role_versions": 0,
        }
        record["spec"]["reports"][0]["contract_id"] = "invalid"
        before = deepcopy(record)
        with pytest.raises(ValueError):
            validate(RoleBackup((record,), fake_port, (), ()))
        assert record == before
    finally:
        engine.dispose()


def test_next_opening_cannot_skip_uncovered_observation_history():
    with pytest.raises(ValueError, match="未覆盖观测"):
        next_opening(
            NextOpening(
                product_id="SHFE.AU",
                trading_time=fixture().spec.trading_time,
                observation_end="2024-01-01T15:00:00+08:00",
                available_at="2024-01-01T16:00:00+08:00",
            )
        )


def test_role_dependency_and_export_contract():
    from asterion_bindings.plugin_host import PluginHost

    from asterion.distribution import builtin_plugins

    with pytest.raises(ValueError, match="Missing required plugin"):
        PluginHost(tuple(p for p in builtin_plugins() if p.id != "asterion.data"))


def test_retrospective_cutoff_cannot_read_later_identity_evidence():
    value = fixture().spec.model_dump(mode="json")
    value["catalog"]["contracts"][0]["provenance"].update(
        observed_at="2026-01-01T00:00:00Z", available_at="2026-01-01T00:00:00Z"
    )
    spec = RoleSpec.model_validate(value)
    version = RoleVersion(id=role_id(spec), spec=spec)
    with pytest.raises(ValueError, match="身份资料"):
        resolve(
            version,
            query(
                version,
                mode="retrospective",
                information_at="2025-05-01T00:00:00Z",
                explanation="历史回看",
            ),
        )
