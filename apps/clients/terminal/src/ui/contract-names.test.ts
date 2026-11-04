import { create } from "@bufbuild/protobuf";
import { afterEach, expect, test } from "vitest";
import { CatalogContractSchema } from "../generated/asterion/market_pb";
import { setLocale } from "../i18n";
import { catalogName, identityName } from "./contract-names";

afterEach(() => setLocale("zh-CN"));

test("contracts are named in the interface's language", () => {
  const rebar = create(CatalogContractSchema, {
    instrument: { venue: "SHFE", symbol: "rb2610" },
    product: "rb",
    name: "螺纹钢2610",
  });
  expect(catalogName(rebar)).toBe("螺纹钢2610");
  expect(identityName("SHFE/cu/2023-10")).toBe("沪铜2310");
  setLocale("en-US");
  expect(catalogName(rebar)).toBe("Rebar 2610");
  expect(identityName("SHFE/cu/2023-10", "沪铜2310")).toBe("Copper 2310");
});
