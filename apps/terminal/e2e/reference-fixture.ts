import { createHash } from "node:crypto";

export function coverageIdentity(
  versionId = "fixed-contracts",
  symbol = "RB2405.SHF",
) {
  const source = "tushare";
  const provenance = {
    source,
    source_version: versionId,
    observed_at: "2026-01-01T00:00:00Z",
    available_at: "2026-01-01T00:01:00Z",
  };
  const id = "SHFE.RB.202405.20230516";
  const catalog = {
    schema_version: 2,
    inputs: [{ version_id: versionId, source, checksum: "a".repeat(64) }],
    products: [
      {
        id: "SHFE.RB",
        exchange: "SHFE",
        name: "Fixture RB",
        currency: "CNY",
        provenance,
      },
    ],
    contracts: [
      {
        id,
        product_id: "SHFE.RB",
        delivery_month: "2024-05",
        listed_on: "2023-05-16",
        last_trade_on: "2024-05-15",
        last_delivery_on: null,
        provenance,
      },
    ],
    symbols: [
      {
        source,
        symbol,
        contract_id: id,
        valid_from: "2023-05-16",
        valid_until: "2024-05-15",
        provenance,
      },
    ],
  };
  function sorted(value: unknown): unknown {
    if (Array.isArray(value)) return value.map(sorted);
    if (value && typeof value === "object")
      return Object.fromEntries(
        Object.entries(value)
          .sort(([a], [b]) => a.localeCompare(b))
          .map(([k, v]) => [k, sorted(v)]),
      );
    return value;
  }
  return {
    catalog_id: createHash("sha256")
      .update(JSON.stringify(sorted(catalog)))
      .digest("hex"),
    catalog,
    source,
    symbol,
    information_at: provenance.available_at,
  };
}

export function fileIdentity(contract = "SHFE.rb2405") {
  const fixture = coverageIdentity();
  const catalog = fixture.catalog;
  catalog.inputs = [];
  if (contract === "SHFE.rb2610") {
    const id = "SHFE.RB.202610.20240102";
    Object.assign(catalog.contracts[0], { id, delivery_month: "2026-10", listed_on: "2024-01-02", last_trade_on: "2026-10-15" });
    Object.assign(catalog.symbols[0], { symbol: "RB2610.SHF", contract_id: id, valid_from: "2024-01-02", valid_until: "2026-10-15" });
  }
  // The API computes the release digest when this fixture is published.
  return { catalog, bindings: [{ contract, source: "tushare", symbol: catalog.symbols[0].symbol }] };
}
