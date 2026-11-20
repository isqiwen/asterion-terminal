import { create } from "@bufbuild/protobuf";
import { render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import { StrategyCard } from "./StrategyCard";
import { feature } from "./feature";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { Side } from "../../src/generated/asterion/common_pb";
import {
  InstanceState,
  InstanceStatusSchema,
  SessionSchema,
} from "../../src/generated/asterion/trading_pb";

registerTerminalFeatures([feature]);

test("a running strategy shows its state, latest bars, targets against positions and latest events", () => {
  const rb = { exchange: "SHFE", symbol: "rb2610" };
  const instance = create(InstanceStatusSchema, {
    state: InstanceState.READY,
    lastBarMs: BigInt(Date.UTC(2026, 10, 19, 6, 31)),
    instance: { id: "soak", packageName: "ma_cross.MaCross", instruments: [rb] },
    lastDecision: {
      sequence: 7n,
      targets: [{ instrument: rb, size: { case: "lots", value: 2n } }],
    },
    events: ["first", "second", "third", "fourth"].map((text, at) => ({
      timeMs: BigInt(at),
      text,
    })),
  });
  const live = create(SessionSchema, {
    positions: [{ instrument: rb, side: Side.BUY, todayLots: { text: "1" } }],
  });
  render(<StrategyCard instance={instance} live={live} open={() => {}} />);
  const card = within(screen.getByRole("region", { name: "运行中的策略" }));
  expect(card.getByText("策略已就绪")).toBeTruthy();
  expect(card.getByText(/^最近 K 线 \d\d-\d\d \d\d:\d\d$/)).toBeTruthy();
  const row = card.getByRole("row", { name: /SHFE\.rb2610/ });
  expect(row.textContent).toBe("SHFE.rb26102" + "1");
  expect(row.dataset.off).toBe("true");
  // The three latest, newest first.
  const events = card.getAllByRole("listitem").map(item => item.textContent?.split(" · ")[1]);
  expect(events).toEqual(["fourth", "third", "second"]);
});
