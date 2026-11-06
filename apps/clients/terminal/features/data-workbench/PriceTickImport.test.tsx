import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, expect, test, vi } from "vitest";
import { setLocale } from "../../src/i18n";
import { BackendError } from "../../src/i18n/errors";
import { PriceTickImport } from "./PriceTickImport";

const request = vi.hoisted(() => vi.fn());
vi.mock("../contract", async importOriginal => ({
  ...(await importOriginal<typeof import("../contract")>()),
  marketData: request,
  translate: (_namespace: string, key: string, values?: Record<string, unknown>) =>
    key.replace(/\{(\w+)\}/g, (_, name: string) => String(values?.[name])),
}));
afterEach(() => vi.resetAllMocks());
const data = {
  exchange: "SHFE",
  rows: [
    {
      contract: "SHFE/rb/2015-05",
      effectiveFrom: "2014-05-16",
      effectiveUntil: "2015-05-16",
      priceTick: "0.00000001",
      sourceUrl: "https://www.shfe.com.cn/test-rule",
    },
  ],
};
function choose(text = JSON.stringify(data)) {
  const file = new File([text], "historical-rules.json", { type: "application/json" });
  Object.defineProperty(file, "text", { value: async () => text });
  fireEvent.change(screen.getByLabelText("历史规则文件（JSON）"), { target: { files: [file] } });
}

test("historical rules are reviewed before an exact decimal is imported into Market", async () => {
  const imported = vi.fn();
  request.mockResolvedValue({ provider: "rules", version: "new-fixed-version" });
  render(<PriceTickImport onImported={imported} />);
  choose();
  await screen.findByText("SHFE · 1 条 · 2014-05-16 至 2015-05-16（不含）");
  expect(screen.getByText("将替换该交易所已导入的历史规则，已固定的旧版本不变。")).toBeTruthy();
  expect(request).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "导入" }));
  await screen.findByText("已导入");
  expect(request).toHaveBeenCalledWith({
    case: "importPriceTicks",
    value: {
      exchange: "SHFE",
      rows: [{ ...data.rows[0], priceTick: expect.objectContaining({ text: "0.00000001" }) }],
    },
  });
  expect(imported).toHaveBeenCalledWith({ provider: "rules", version: "new-fixed-version" });
});

test("a localized rejection keeps its diagnostic and the reviewed file for an explicit retry", async () => {
  const imported = vi.fn();
  const copy = vi.fn().mockResolvedValue(undefined);
  vi.stubGlobal("navigator", { clipboard: { writeText: copy } });
  const diagnostic = "overlapping imported price tick rules: SHFE/rb/2015-05";
  request.mockRejectedValueOnce(new BackendError("invalid_request", diagnostic));
  render(<PriceTickImport onImported={imported} />);
  choose();
  await screen.findByText("historical-rules.json");
  fireEvent.click(screen.getByRole("button", { name: "导入" }));
  await screen.findByText("同一合约的历史报价单位生效区间重叠，请核对导入文件。");
  fireEvent.click(screen.getByRole("button", { name: "复制诊断" }));
  expect(copy).toHaveBeenCalledWith(`invalid_request: ${diagnostic}`);
  act(() => setLocale("en-US"));
  expect(
    screen.getByText(
      "Imported price tick intervals overlap for the same contract. Check the file.",
    ),
  ).toBeTruthy();
  await waitFor(() =>
    expect((screen.getByRole("button", { name: "导入" }) as HTMLButtonElement).disabled).toBe(
      false,
    ),
  );
  expect(screen.getByText("historical-rules.json")).toBeTruthy();
  expect(request).toHaveBeenCalledTimes(1);
  expect(imported).not.toHaveBeenCalled();
  request.mockResolvedValueOnce({ provider: "rules", version: "accepted" });
  fireEvent.click(screen.getByRole("button", { name: "导入" }));
  await screen.findByText("已导入");
  expect(request).toHaveBeenCalledTimes(2);
});

test("an invalid historical rules file cannot submit an import", async () => {
  render(<PriceTickImport onImported={vi.fn()} />);
  choose("{ invalid JSON");
  await screen.findByText("文件不是有效的 JSON。");
  choose(JSON.stringify({ exchange: "SHFE", rows: [] }));
  await screen.findByText("文件需要交易所和非空的历史规则列表。");
  await waitFor(() =>
    expect((screen.getByLabelText("历史规则文件（JSON）") as HTMLInputElement).disabled).toBe(
      false,
    ),
  );
  expect(screen.queryByRole("button", { name: "导入" })).toBeNull();
  expect(request).not.toHaveBeenCalled();
});
