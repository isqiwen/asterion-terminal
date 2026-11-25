import { create } from "@bufbuild/protobuf";
import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { StrategyCard } from "./StrategyCard";
import { feature } from "./feature";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { Side } from "../../src/generated/asterion/common_pb";
import {
  InstanceEventKind,
  InstanceState,
  InstanceStatusSchema,
  SessionSchema,
} from "../../src/generated/asterion/trading_pb";

registerTerminalFeatures([feature]);

test("a running strategy shows its state, latest bars, target drift, events and direct controls", async () => {
  const rb = { exchange: "SHFE", symbol: "rb2610" };
  const instance = create(InstanceStatusSchema, {
    state: InstanceState.READY,
    completedSteps: 7n,
    lastBarMs: BigInt(Date.UTC(2026, 10, 19, 6, 31)),
    instance: { id: "soak", packageName: "ma_cross.MaCross", instruments: [rb] },
    lastDecision: {
      sequence: 7n,
      targets: [{ instrument: rb, size: { case: "lots", value: 2n } }],
    },
    events: [
      { timeMs: 0n, text: "first", kind: InstanceEventKind.UNSPECIFIED },
      { timeMs: 1n, text: "second", kind: InstanceEventKind.ORDER_SENT },
      { timeMs: 2n, text: "third", kind: InstanceEventKind.PAUSED },
      { timeMs: 3n, text: "fourth", kind: InstanceEventKind.BATCH_ABANDONED },
    ],
  });
  const live = create(SessionSchema, {
    positions: [{ instrument: rb, side: Side.BUY, todayLots: { text: "1" } }],
  });
  const pause = vi.fn(async () => true);
  const stop = vi.fn(async () => true);
  render(
    <StrategyCard
      instance={instance}
      live={live}
      open={() => {}}
      pause={pause}
      stop={stop}
      flatten={vi.fn(async () => true)}
    />,
  );
  const card = within(screen.getByRole("region", { name: "运行中的策略" }));
  expect(card.getByText("策略已就绪")).toBeTruthy();
  expect(card.getByText(/^最近 K 线 \d\d-\d\d \d\d:\d\d$/)).toBeTruthy();
  const row = card.getByRole("row", { name: /SHFE\.rb2610/ });
  expect(
    within(row)
      .getAllByRole("cell")
      .map(cell => cell.textContent),
  ).toEqual(["SHFE.rb2610", "2", "1", "+1"]);
  expect(row.dataset.off).toBe("true");
  // The three latest, newest first, with severity tones.
  const events = card.getAllByRole("listitem");
  expect(events.map(item => item.textContent?.split(" · ")[1])).toEqual([
    "fourth",
    "third",
    "second",
  ]);
  expect(events[0]!.dataset.tone).toBe("danger");
  expect(events[1]!.dataset.tone).toBe("attention");
  expect(events[2]!.dataset.tone).toBe("ok");

  fireEvent.click(card.getByRole("button", { name: "暂停新增" }));
  expect(pause).toHaveBeenCalledWith("soak", true);

  fireEvent.click(card.getByRole("button", { name: "停止策略并撤单" }));
  expect(stop).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "确认停止并撤单" }));
  await waitFor(() => expect(stop).toHaveBeenCalledWith("soak"));
});
