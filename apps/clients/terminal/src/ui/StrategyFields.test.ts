import { describe, expect, it } from "vitest";
import { strategiesOf, strategyDefaults } from "./StrategyFields";

describe("the strategies a form submits", () => {
  it("is one strategy when every window holds one value", () => {
    expect(strategiesOf({ ...strategyDefaults, fast: "5", slow: "20" })).toEqual([
      { quantity: "1", sides: "both", rule: { kind: "moving_average", fast: 5, slow: 20 } },
    ]);
  });
  it("is every combination of the windows, in the order written, without those the rule cannot use", () => {
    const rules = (draft: Partial<typeof strategyDefaults>) =>
      strategiesOf({ ...strategyDefaults, ...draft }).map(strategy => strategy.rule);
    // A fast average is shorter than its slow one: 20/20 and 20/10 are left out.
    expect(rules({ fast: "5, 20", slow: "20,10 ,60" })).toEqual([
      { kind: "moving_average", fast: 5, slow: 20 },
      { kind: "moving_average", fast: 5, slow: 10 },
      { kind: "moving_average", fast: 5, slow: 60 },
      { kind: "moving_average", fast: 20, slow: 60 },
    ]);
    // An exit channel is no longer than its entry.
    expect(rules({ kind: "breakout", entry: "10,20", exit: "10,20" })).toEqual([
      { kind: "breakout", entry: 10, exit: 10 },
      { kind: "breakout", entry: 20, exit: 10 },
      { kind: "breakout", entry: 20, exit: 20 },
    ]);
    // A band width stays the text that was written: it is a decimal.
    expect(rules({ kind: "reversion", window: "20", width: "1.5,2" })).toEqual([
      { kind: "reversion", window: 20, width: "1.5" },
      { kind: "reversion", window: 20, width: "2" },
    ]);
    // A rule that ranks contracts varies all three of its fields.
    expect(
      rules({ kind: "cross_momentum", lookback: "5,20", rebalance: "5", count: "1,2" }),
    ).toEqual([
      { kind: "cross_momentum", lookback: 5, rebalance: 5, count: 1 },
      { kind: "cross_momentum", lookback: 5, rebalance: 5, count: 2 },
      { kind: "cross_momentum", lookback: 20, rebalance: 5, count: 1 },
      { kind: "cross_momentum", lookback: 20, rebalance: 5, count: 2 },
    ]);
    expect(rules({ fast: "20", slow: "5" })).toEqual([]);
  });
});
