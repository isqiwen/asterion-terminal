import { create } from "@bufbuild/protobuf";
import { afterEach, expect, test } from "vitest";
import { CatalogContractSchema } from "../generated/asterion/market_pb";
import { setLocale } from "../i18n";
import { catalogName, identityName, resolveProductCode, warehouseName } from "./contract-names";

afterEach(() => setLocale("zh-CN"));

test("contracts are named in the interface's language", () => {
  const rebar = create(CatalogContractSchema, {
    instrument: { exchange: "SHFE", symbol: "rb2610" },
    product: "rb",
    name: "螺纹钢2610",
  });
  expect(catalogName(rebar)).toBe("螺纹钢2610");
  expect(identityName("SHFE/cu/2023-10")).toBe("沪铜2310");
  expect(warehouseName("CFFEX/ic")).toBe("中证500");
  expect(warehouseName("CFFEX/ic/2026-11")).toBe("中证5002611");
  expect(warehouseName("GFEX/li/2027-06")).toBe("氢氧化锂2706");
  setLocale("en-US");
  expect(catalogName(rebar)).toBe("Rebar 2610");
  expect(identityName("SHFE/cu/2023-10", "沪铜2310")).toBe("Copper 2310");
  expect(warehouseName("CFFEX/ic")).toBe("CSI 500");
  expect(warehouseName("GFEX/li")).toBe("Lithium Hydroxide");
});

test("product search text resolves unambiguous Chinese or English names to product codes", () => {
  expect(resolveProductCode("SHFE", "沪银")).toBe("ag");
  expect(resolveProductCode("SHFE", "螺纹")).toBe("rb");
  expect(resolveProductCode("SHFE", "Silver")).toBe("ag");
  expect(resolveProductCode("SHFE", "铝")).toBe("铝");
  expect(resolveProductCode("SHFE", "rb2610")).toBe("rb2610");
  expect(resolveProductCode("GFEX", "氢氧化锂")).toBe("li");
});
