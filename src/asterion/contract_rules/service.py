"""Append-only, content-addressed contract rules owned by this plugin."""

from asterion_bindings.rules import RuleSpec, RuleVersion, rule_id
from sqlalchemy import JSON, Column, String, Table, select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert

from asterion.contract_rules.mapping import SourceMapping
from asterion.contract_rules.settlement import SettlementMapping
from asterion.data.public import VersionAccess
from asterion.platform.store import metadata

rules = Table(
    "contract_rule_versions",
    metadata,
    Column("id", String, primary_key=True),
    Column("spec", JSON, nullable=False),
)


class Rules:
    def __init__(self, storage, versions: VersionAccess):
        self.mapping = SourceMapping(versions)
        self.settlement = SettlementMapping(versions)
        self.storage = storage
        storage.initialize(rules)

    def save(self, spec: RuleSpec):
        self.mapping.validate(spec)
        self.settlement.validate(spec)
        version = RuleVersion(id=rule_id(spec), spec=spec)
        insert = pg_insert if self.storage.dialect.name == "postgresql" else sqlite_insert
        with self.storage.begin() as conn:
            conn.execute(
                insert(rules)
                .values(id=version.id, spec=spec.model_dump(mode="json"))
                .on_conflict_do_nothing(index_elements=["id"])
            )
        return self.read(version.id)

    def read(self, identifier):
        with self.storage.connect() as conn:
            value = conn.execute(select(rules).where(rules.c.id == identifier)).mappings().first()
        if value is None:
            raise ValueError("规则版本不存在，请先保存合约规则")
        return RuleVersion.model_validate(dict(value))

    def list(self):
        with self.storage.connect() as conn:
            return [
                RuleVersion.model_validate(dict(row))
                for row in conn.execute(select(rules).order_by(rules.c.id)).mappings()
            ]
