"""Explicit file-code bindings to immutable, attributed actual-contract evidence."""

from datetime import date

from pydantic import AwareDatetime, Field, model_validator

from asterion.data.reference import (
    Contract,
    ReferenceCatalog,
    ReferenceModel,
    ResolutionRequest,
    validate_market_code,
)


class FileSymbol(ReferenceModel):
    contract: str = Field(min_length=1, max_length=100)
    source: str = Field(min_length=1, max_length=64)
    symbol: str = Field(min_length=1, max_length=64)


class ImportIdentity(ReferenceModel):
    catalog_id: str = Field(pattern=r"^[0-9a-f]{64}$")
    catalog: ReferenceCatalog
    information_at: AwareDatetime
    bindings: tuple[FileSymbol, ...] = Field(min_length=1, max_length=10000)

    @model_validator(mode="after")
    def consistency(self):
        from asterion.data.reference_store import catalog_digest

        if catalog_digest(self.catalog) != self.catalog_id:
            raise ValueError("导入合约目录指纹不一致")
        if len({item.contract for item in self.bindings}) != len(self.bindings):
            raise ValueError("文件合约代码映射重复")
        pairs = {(item.source, item.symbol) for item in self.catalog.symbols}
        if any((item.source, item.symbol) not in pairs for item in self.bindings):
            raise ValueError("文件代码映射不在固定目录中")
        return self

    def resolve(self, contract: str, day: date) -> Contract:
        binding = next((item for item in self.bindings if item.contract == contract), None)
        if binding is None:
            raise ValueError("文件合约缺少明确的身份映射")
        result = self.catalog.resolve(
            ResolutionRequest(
                source=binding.source,
                symbol=binding.symbol,
                trading_day=day,
                information_at=self.information_at,
            )
        ).contract
        validate_market_code(contract, result)
        return result

    def validate_rows(self, rows: list[dict]):
        if {row["contract"] for row in rows} != {item.contract for item in self.bindings}:
            raise ValueError("身份映射必须与文件合约集合完全一致")
        for row in rows:
            self.resolve(row["contract"], date.fromisoformat(str(row["trading_day"])))

    def validate_inputs(self, reader):
        from asterion.data.reference_source import validate_catalog_input

        for item in self.catalog.inputs:
            try:
                result = reader(item.version_id, limit=1)
            except KeyError:
                raise ValueError("导入身份目录的固定资料版本不存在") from None
            validate_catalog_input(item, result["version"]["manifest"])
