import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type { TerminalContext } from "../contract";
import { Coverage } from "./Coverage";

const request = vi.hoisted(() => vi.fn());
vi.mock("../contract", async importOriginal => ({
  ...(await importOriginal<typeof import("../contract")>()),
  lab: request,
  translate: (_namespace: string, key: string) => key,
}));

test("switching providers keeps a running source independent and submits the selected source", async () => {
  request.mockImplementation(async (body: { case: string; value: { provider?: string } }) => {
    switch (body.case) {
      case "sources":
        return {
          sources: [
            { id: "tushare", tables: ["daily"], exchanges: ["SHFE"], requiresCredential: true },
            { id: "akshare", tables: ["daily"], exchanges: ["SHFE"] },
            { id: "shfe", tables: ["settlement"], exchanges: ["SHFE"] },
          ],
        };
      case "summary":
        return { datasetVersion: "", tables: [] };
      case "coverage":
        return { products: [] };
      case "tasks":
        return {
          tasks: [
            {
              id: "first",
              provider: "tushare",
              table: "daily",
              state: "running",
              daysDone: 0,
              daysTotal: 1,
            },
          ],
        };
      default:
        return {};
    }
  });
  const trade = vi.fn();
  render(<Coverage {...({ trade } as unknown as TerminalContext)} />);
  await screen.findByRole("option", { name: "akshare" });
  await waitFor(() =>
    expect((screen.getByRole("button", { name: "开始采集" }) as HTMLButtonElement).disabled).toBe(
      true,
    ),
  );
  fireEvent.change(screen.getByLabelText("数据源"), { target: { value: "akshare" } });
  await waitFor(() =>
    expect((screen.getByRole("button", { name: "开始采集" }) as HTMLButtonElement).disabled).toBe(
      false,
    ),
  );
  fireEvent.click(screen.getByRole("button", { name: "开始采集" }));
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith(
      {
        case: "acquire",
        value: expect.objectContaining({
          provider: "akshare",
          table: "daily",
          exchanges: ["SHFE"],
        }),
      },
      "accepted",
    ),
  );
  expect(trade).not.toHaveBeenCalled();
  fireEvent.change(screen.getByLabelText("数据源"), { target: { value: "shfe" } });
  expect((screen.getByLabelText("表") as HTMLSelectElement).value).toBe("settlement");
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith(
      { case: "summary", value: { provider: "shfe" } },
      "summary",
    ),
  );
});
