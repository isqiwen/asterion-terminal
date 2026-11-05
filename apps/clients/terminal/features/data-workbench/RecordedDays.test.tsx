import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { RecordedDays } from "./RecordedDays";

const request = vi.hoisted(() => vi.fn());
vi.mock("../contract", async importOriginal => ({
  ...(await importOriginal<typeof import("../contract")>()),
  marketData: request,
  getLocale: () => "zh-CN",
  translate: (_namespace: string, key: string, values?: Record<string, unknown>) =>
    key.replace(/\{(\w+)\}/g, (_, name: string) => String(values?.[name])),
}));

// 2026-10-30 Beijing: watched 09:00–10:00, then 10:30–11:00 after a restart.
const at = (clock: string) => BigInt(Date.parse(`2026-10-30T${clock}:00+08:00`));
request.mockResolvedValue({
  days: [
    {
      tradingDay: "2026-10-30",
      segments: [
        {
          instance: "a",
          number: 1n,
          firstReceivedMs: at("09:00"),
          lastReceivedMs: at("10:00"),
          ticks: 1200n,
          end: "stopped",
        },
        {
          instance: "b",
          number: 1n,
          firstReceivedMs: at("10:30"),
          lastReceivedMs: at("11:00"),
          ticks: 600n,
          end: "",
        },
      ],
    },
  ],
});

test("a trading day shows its two observation segments and the gap between them", async () => {
  render(<RecordedDays />);
  const day = await screen.findByRole("row", { name: /2026-10-30/ });
  expect(day.textContent).toContain("1 处，共 30 分钟");
  fireEvent.click(day);
  const segments = screen.getByRole("table", { name: "2026-10-30 的观测段" });
  const [, first, second] = within(segments).getAllByRole("row");
  expect(first!.textContent).toContain("09:00:00");
  expect(first!.textContent).toContain("stopped");
  expect(second!.textContent).toContain("10:30:00");
  expect(second!.textContent).toContain("进程停止前未记录原因");
  expect(second!.textContent).toContain("30 分钟");
});
