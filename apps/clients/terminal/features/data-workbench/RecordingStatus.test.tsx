import { act, render, screen } from "@testing-library/react";
import { afterEach, expect, test, vi } from "vitest";
import type { Snapshot } from "../../src/bridge/client";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { readySnapshot } from "../../src/testing/core";
import type { TerminalContext } from "../contract";
import { RecordingStatus } from "./RecordingStatus";
import { feature } from "./feature";

registerTerminalFeatures([feature]);
afterEach(() => vi.useRealTimers());

const snapshot = (instance: string): Snapshot => ({
  ...readySnapshot(),
  market: { ...readySnapshot().market!, instance_id: instance, transport_online: true },
});
const reply = (instance: string, extra = {}): Snapshot => ({
  ...snapshot(instance),
  recording: { instance, phase: "connected", rows: "1234", ...extra },
});

function mount(instance: string) {
  const replies: ((result: Snapshot) => void)[] = [];
  const query = vi.fn<TerminalContext["query"]>(
    () => new Promise(resolve => replies.push(resolve)),
  );
  const context: TerminalContext = {
    snapshot: snapshot(instance),
    query,
    busy: false,
    error: "",
    trade: async () => {},
    navigate: () => {},
    openSettings: () => {},
  };
  const view = render(<RecordingStatus context={context} />);
  return {
    query,
    replies,
    select: (value: Snapshot) =>
      view.rerender(<RecordingStatus context={{ ...context, snapshot: value }} />),
  };
}

test("status reads do not overlap, and switching or closing a Market discards pending replies", async () => {
  vi.useFakeTimers();
  const { replies, query, select } = mount("first");
  expect(query).toHaveBeenCalledWith("market.recording");
  await act(async () => vi.advanceTimersByTime(9000));
  expect(replies).toHaveLength(1);
  select(snapshot("second"));
  expect(replies).toHaveLength(2);
  await act(async () => replies[0](reply("first")));
  expect(screen.queryByText("1,234")).toBeNull();
  await act(async () => replies[1](reply("second")));
  expect(screen.getByText("1,234")).toBeDefined();
  await act(async () => vi.advanceTimersByTime(3000));
  expect(replies).toHaveLength(3);
  select({ ...snapshot("second"), market: null });
  await act(async () => replies[2](reply("second")));
  expect(screen.queryByText("1,234")).toBeNull();
  expect(screen.getByText("连接远程 Market 后查看录制状态。")).toBeDefined();
  await act(async () => vi.advanceTimersByTime(9000));
  expect(replies).toHaveLength(3);
});

test("write failures preserve exact counts, and a reply from another instance cannot show success", async () => {
  vi.useFakeTimers();
  const { replies, select } = mount("market");
  await act(async () =>
    replies[0](
      reply("market", {
        lost: "9007199254740993",
        writeError: "fixture disk full",
        mergeError: "fixture merge failed",
      }),
    ),
  );
  expect(
    screen
      .getAllByRole("alert")
      .map(item => item.textContent)
      .join(" "),
  ).toContain("9,007,199,254,740,993");
  expect(
    screen
      .getAllByRole("alert")
      .map(item => item.textContent)
      .join(" "),
  ).toContain("fixture merge failed");
  await act(async () => vi.advanceTimersByTime(3000));
  await act(async () => replies[1](reply("another")));
  expect(screen.getByText("录制状态不属于当前行情服务")).toBeDefined();
  expect(screen.queryByText("行情已连接")).toBeNull();
  select({
    ...snapshot("market"),
    market: { ...snapshot("market").market!, transport_online: false },
  });
  expect(screen.getByText("行情服务失联，录制状态未知。")).toBeDefined();
  await act(async () => vi.advanceTimersByTime(9000));
  expect(replies).toHaveLength(2);
});
