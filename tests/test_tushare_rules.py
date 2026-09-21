from copy import deepcopy

import pytest
from fastapi.testclient import TestClient
from rules_support import rule_version
from test_data_sync import MASTER, context, prepared, request  # noqa: F401
from test_research import version_access

from asterion.api.app import create_app
from asterion.contract_rules.mapping import MappingRequest, SourceMapping
from asterion.contract_rules.public import RuleSpec
from asterion.contract_rules.service import Rules
from asterion.data.providers.tushare import contract_multiplier
from asterion.data.public import VersionAccess
from asterion.distribution import strategy_catalog
from asterion.distribution_storage import rule_storage
from asterion.platform.config import Settings


def publish_contracts(context, monkeypatch):  # noqa: F811
    def enrich(rows):
        rows[0].update(quote_unit_desc="1元/吨", multiplier=None)
        return rows

    job, content = prepared(context, monkeypatch, request("contracts"), enrich)
    return context[2].publish(job["id"], job["token"], content)


def test_mapping_uses_verified_fixed_version_and_preserves_evidence(context, monkeypatch):  # noqa: F811
    engine, _, sync, root = context
    published = publish_contracts(context, monkeypatch)
    service = Rules(rule_storage(engine), version_access(engine, root))
    preview = service.mapping.preview(
        MappingRequest(version_id=published["id"], contract_id="SHFE.RB.202610.20230101")
    )
    assert preview.contract.id == "SHFE.RB.202610.20230101"
    assert preview.suggested_multiplier == "10"
    assert preview.basis.quote_unit_desc == "1元/吨"
    assert preview.basis.symbol == "RB2610.SHF"
    assert preview.basis.checksum == published["manifest"]["checksum"]
    assert preview.basis.connection_id == published["manifest"]["origin"]["connection_id"]
    assert "最小变动价位" in preview.missing and "生效期间" in preview.missing
    spec = rule_version("SHFE.rb2610").spec.model_dump(mode="json")
    spec["basis"] = preview.basis.model_dump(mode="json")
    spec["contract"] = preview.contract.model_dump(mode="json")
    version = service.save(RuleSpec.model_validate(spec))
    assert version.spec.basis == preview.basis
    assert service.save(version.spec) == version
    for field, value in [
        ("checksum", "0" * 64),
        ("name", "forged"),
        ("connection_id", "c_" + "0" * 32),
        ("per_unit", "1000"),
    ]:
        changed = deepcopy(spec)
        changed["basis"][field] = value
        with pytest.raises(ValueError, match="快照"):
            service.save(RuleSpec.model_validate(changed))
    assert len(service.list()) == 1
    # A damaged file cannot be mapped even if database metadata remains present.
    file = root / sync.library.preview(published["id"])["version"]["manifest"].get(
        "path", "missing"
    )
    from sqlalchemy import select

    from asterion.data.library import versions

    with engine.connect() as conn:
        manifest = conn.execute(
            select(versions.c.manifest).where(versions.c.id == published["id"])
        ).scalar_one()
    file = root / manifest["path"]
    file.write_bytes(b"corrupted")
    with pytest.raises(ValueError, match="校验和"):
        service.mapping.preview(
            MappingRequest(version_id=published["id"], contract_id="SHFE.RB.202610.20230101")
        )


def source_fixture():
    return {
        "version": {
            "id": "fixed",
            "created_at": 1,
            "manifest": {
                "source": "tushare",
                "state": "PUBLISHED",
                "observed_at": "2020-01-01T00:00:00Z",
                "available_at": "2020-01-01T00:00:00Z",
                "layer": "STANDARD",
                "type": {"id": "futures.contracts", "schema_version": 4},
                "checksum": "a" * 64,
                "origin": {"connection_id": "c_" + "a" * 32},
            },
        },
        "total": 1,
        "rows": [
            {
                "contract": "SHFE.RB2610",
                "product": "RB",
                "currency": "CNY",
                "delivery_month": "2026-10",
                "last_delivery_on": None,
                "rules_status": "INCOMPLETE",
                "suggested_multiplier": "10",
                "multiplier_note": "fixture units",
                "exchange": "SHFE",
                "symbol": "RB2610.SHF",
                "name": "fixture",
                "listed": "2023-01-01",
                "delisted": "2026-10-15",
                "trade_unit": "吨",
                "per_unit": "10",
                "quote_unit": "元/吨",
                "multiplier": None,
                "quote_unit_desc": "1元/吨",
            }
        ],
    }


def mapper(source):
    return SourceMapping(VersionAccess(lambda *args, **kwargs: source, lambda _: None))


@pytest.mark.parametrize(
    "mutation",
    [
        "source",
        "type",
        "layer",
        "schema",
        "duplicate",
        "missing",
        "truncated",
        "demo",
        "field",
        "origin",
    ],
)
def test_mapping_refuses_wrong_or_ambiguous_evidence(mutation):
    source = source_fixture()
    manifest = source["version"]["manifest"]
    if mutation == "source":
        manifest["source"] = ""
    if mutation == "type":
        manifest["type"]["id"] = "futures.daily"
    if mutation == "layer":
        manifest["layer"] = "RAW"
    if mutation == "schema":
        manifest["type"]["schema_version"] = 999
    if mutation == "duplicate":
        source["rows"] *= 2
        source["total"] = 2
    if mutation == "missing":
        source["rows"] = []
        source["total"] = 0
    if mutation == "truncated":
        source["total"] = 10001
    if mutation == "demo":
        manifest["demo"] = True
    if mutation == "field":
        source["rows"][0].pop("quote_unit_desc")
    if mutation == "origin":
        manifest.pop("origin")
    with pytest.raises(ValueError):
        mapper(source).preview(
            MappingRequest(version_id="fixed", contract_id="SHFE.RB.202610.20230101")
        )


@pytest.mark.parametrize("symbol,expected", [("IF2610", "300"), ("T2612", None)])
def test_financial_contract_mapping_never_guesses_treasury_price_conversion(symbol, expected):
    source = source_fixture()
    source["rows"][0].update(
        exchange="CFFEX",
        symbol=symbol + ".CFX",
        contract="CFFEX." + symbol,
        product=symbol[:-4],
        delivery_month="2026-" + symbol[-2:],
        multiplier="300",
        per_unit="1000000",
        quote_unit="百元报价",
    )
    source["rows"][0].update(contract_multiplier(source["rows"][0]))
    result = mapper(source).preview(
        MappingRequest(
            version_id="fixed", contract_id=f"CFFEX.{symbol[:-4]}.2026{symbol[-2:]}.20230101"
        )
    )
    assert result.suggested_multiplier == expected


@pytest.mark.parametrize(
    "units,quantity", [("美元/吨", "10"), ("元/吨", None), ("元/吨", "0"), ("元/吨", "NaN")]
)
def test_unknown_units_or_invalid_quantity_are_left_unfilled(units, quantity):
    source = source_fixture()
    source["rows"][0].update(quote_unit=units, per_unit=quantity)
    source["rows"][0].update(contract_multiplier(source["rows"][0]))
    assert (
        mapper(source)
        .preview(MappingRequest(version_id="fixed", contract_id="SHFE.RB.202610.20230101"))
        .suggested_multiplier
        is None
    )


def test_mapping_api_and_reference_hook(context, monkeypatch):  # noqa: F811
    engine, _, _, root = context
    published = publish_contracts(context, monkeypatch)
    settings = Settings(token=MASTER, data_root=root, require_account=False)
    with TestClient(create_app(settings, engine)) as client:
        body = {"version_id": published["id"], "contract_id": "SHFE.RB.202610.20230101"}
        assert client.post("/api/v1/contract-rules/source/preview", json=body).status_code == 401
        client.headers["Authorization"] = "Bearer " + MASTER
        preview = client.post("/api/v1/contract-rules/source/preview", json=body)
        assert preview.status_code == 200
        spec = rule_version("SHFE.rb2610").spec.model_dump(mode="json")
        spec["basis"] = preview.json()["basis"]
        spec["contract"] = preview.json()["contract"]
        assert client.post("/api/v1/contract-rules", json=spec).status_code == 200
        status = client.get(f"/api/v1/data/versions/{published['id']}/lifecycle")
        assert status.status_code == 200
        assert status.json()["references"]["contract_rules"] == 1


from test_research import services as research_services  # noqa: F401


def test_mapped_rules_replay_without_source_access(research_services):  # noqa: F811
    from storage_support import raw_engine, scheduler

    from asterion.contract_rules.public import RuleAccess
    from asterion.platform.serialization import canonical
    from asterion.research.engine import calculate
    from asterion.research.packages import ResearchPackages

    research, body, _ = research_services
    source = source_fixture()
    source["rows"][0]["symbol"] = "RB2405.SHF"
    source["rows"][0].update(
        contract="SHFE.RB2405", delivery_month="2024-05", listed="2023-05-16", delisted="2024-05-15"
    )
    mapping = mapper(source)
    spec = body.rules.spec.model_dump(mode="json")
    preview = mapping.preview(
        MappingRequest(version_id="fixed", contract_id="SHFE.RB.202405.20230516")
    )
    spec["basis"] = preview.basis.model_dump(mode="json")
    spec["contract"] = preview.contract.model_dump(mode="json")
    catalog = Rules(rule_storage(raw_engine(research.engine)), mapping.versions)
    rule = catalog.save(RuleSpec.model_validate(spec))
    job = research.submit(body.model_copy(update={"rules": rule}))
    claimed = scheduler(research.engine).claim("mapped")
    content = canonical(calculate(claimed["payload"], strategy_catalog()))
    research.publish(job["id"], claimed["token"], content)
    packages = ResearchPackages(research)
    exported = packages.export(job["id"], True)
    assert exported["content"]["request"]["rules"]["spec"]["basis"] == spec["basis"]

    def unavailable(*args, **kwargs):
        pytest.fail("Frozen replay must not resolve a source or rule catalogue")

    research.versions = VersionAccess(unavailable, unavailable)
    research.rules = RuleAccess(unavailable)
    imported = packages.receive("offline", exported)
    assert imported["can_replay"]
    replay = packages.replay("offline", imported["id"], "mapped-replay")
    claimed = scheduler(research.engine).claim("replay")
    assert canonical(calculate(claimed["payload"], strategy_catalog())) == content
    research.publish(replay["id"], claimed["token"], content)
    assert research.get(replay["id"])["result"]["reproduction_matches"]


def test_current_tushare_rmb_quotation_suggests_multiplier():
    source = source_fixture()
    source["rows"][0]["quote_unit"] = "人民币元/吨"
    source["rows"][0].update(contract_multiplier(source["rows"][0]))
    assert (
        mapper(source)
        .preview(MappingRequest(version_id="fixed", contract_id="SHFE.RB.202610.20230101"))
        .suggested_multiplier
        == "10"
    )


def test_rules_accept_an_independent_standard_source_without_provider_symbol_assumptions():
    source = source_fixture()
    source["version"]["manifest"]["source"] = "independent-feed"
    row = source["rows"][0]
    row.update(
        symbol="opaque/vendor/instrument-42",
        suggested_multiplier="25",
        multiplier_note="provider quotation definition",
    )
    mapping = mapper(source)
    preview = mapping.preview(
        MappingRequest(version_id="fixed", contract_id="SHFE.RB.202610.20230101")
    )
    assert preview.basis.provider == "independent-feed"
    assert preview.basis.symbol == "opaque/vendor/instrument-42"
    assert preview.suggested_multiplier == "25"
    spec = rule_version("SHFE.rb2610").spec.model_dump(mode="json")
    spec["basis"] = preview.basis.model_dump(mode="json")
    spec["contract"] = preview.contract.model_dump(mode="json")
    mapping.validate(RuleSpec.model_validate(spec))
    spec["basis"]["provider"] = "forged-provider"
    with pytest.raises(ValueError, match="快照"):
        mapping.validate(RuleSpec.model_validate(spec))


def test_mapping_rejects_another_lifecycle_even_with_same_market_code():
    source = source_fixture()
    with pytest.raises(ValueError, match="规范合约身份"):
        mapper(source).preview(
            MappingRequest(version_id="fixed", contract_id="SHFE.RB.202610.20230102")
        )
    preview = mapper(source).preview(
        MappingRequest(version_id="fixed", contract_id="SHFE.RB.202610.20230101")
    )
    spec = rule_version("SHFE.rb2610").spec.model_dump(mode="json")
    spec.update(
        contract=preview.contract.model_dump(mode="json"),
        basis=preview.basis.model_dump(mode="json"),
    )
    spec["basis"]["listed"] = "2023-01-02"
    with pytest.raises(ValueError, match="来源资料不一致"):
        RuleSpec.model_validate(spec)
