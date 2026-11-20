import { describe, expect, it } from "vitest";
import { contractUnits, type DatasetUnits } from "./contractUnits";
import type { FuturesContract } from "../bridge/client";

const contract = (
  symbol: string,
  price_increment: string,
  multiplier = "1000",
): FuturesContract => ({
  venue: "SHFE",
  symbol,
  product: "au",
  delivery_month: "2020-06",
  currency: "CNY",
  price_increment,
  quantity_increment: "1",
  multiplier,
});

describe("contractUnits", () => {
  it("reads the units of one contract as the data give them", () => {
    expect(
      contractUnits([
        {
          contract: contract("au2006", "0.02"),
          first_day: "2020-01-02",
          price_increment_changes: [],
        },
      ]),
    ).toBe("最小变动价位 0.02 · 合约乘数 1000");
  });

  it("joins the months of a series into one schedule of the product", () => {
    // The first month trades through the change, the second begins after it
    // on the new increment: the change is named once.
    const months: DatasetUnits[] = [
      {
        contract: contract("au1912", "0.05"),
        first_day: "2019-11-01",
        price_increment_changes: [{ trading_day: "2019-12-10", value: "0.02" }],
      },
      {
        contract: contract("au2006", "0.02"),
        first_day: "2019-12-12",
        price_increment_changes: [],
      },
    ];
    expect(contractUnits(months)).toBe("最小变动价位 0.05、2019-12-10 起 0.02 · 合约乘数 1000");
    // Months whose lots differ are each named.
    months[1].contract = contract("au2006", "0.02", "500");
    expect(contractUnits(months)).toBe(
      "最小变动价位 0.05、2019-12-10 起 0.02 · 合约乘数 1000、500",
    );
  });
});
