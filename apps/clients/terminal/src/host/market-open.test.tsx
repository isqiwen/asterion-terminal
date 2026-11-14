import { act, render, screen } from "@testing-library/react";
import { afterEach, expect, test, vi } from "vitest";
import { call } from "../bridge/client";
import { ErrorNotice } from "../i18n/errors";
import { MarketProvider, useMarketOpenError } from "./market";

vi.mock("../bridge/client", () => ({ call: vi.fn() }));
afterEach(() => vi.useRealTimers());

function Reason() {
  return <ErrorNotice error={useMarketOpenError()} />;
}

test("a Market that could not be opened at launch is tried again and says why", async () => {
  vi.useFakeTimers();
  vi.mocked(call).mockImplementation(async method => {
    if (method === "market.open") throw new Error("Market 主机不可达");
    return { market_watch: null };
  });
  const opens = () => vi.mocked(call).mock.calls.filter(([method]) => method === "market.open");
  render(
    <MarketProvider configured>
      <Reason />
    </MarketProvider>,
  );
  await act(() => vi.advanceTimersByTimeAsync(9000));
  // The launch itself tried once; the next try waits ten seconds.
  expect(opens()).toHaveLength(0);
  await act(() => vi.advanceTimersByTimeAsync(2000));
  expect(opens()).toHaveLength(1);
  expect(screen.getByText("Market 主机不可达")).toBeTruthy();
  await act(() => vi.advanceTimersByTimeAsync(10500));
  expect(opens()).toHaveLength(2);
});
