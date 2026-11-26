import { act, fireEvent, render, renderHook, screen } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { lab } from "../../src/bridge/lab";
import type { TerminalContext } from "../../src/host/feature-registry";
import { FactorResultSchema, StoredContractsSchema } from "../../src/generated/asterion/lab_pb";
import { setLocale } from "../../src/i18n";
import { feature } from "./feature";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { filterModuleItems, NewModule, useSource, type ModuleItem } from "./LabParts";
import { readUniverses } from "./research-storage";
import { copiedModule, copyStem, factorStrategyTemplate } from "./research-templates";
import { datePlaceholders, SymbolField, SymbolPopover, useStoredCatalog } from "./SymbolPicker";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
registerTerminalFeatures([feature]);

test("IC-weighted strategy generation reports its unsupported mode before saving in both languages", async () => {
  const result = create(FactorResultSchema, {
    evaluation: {
      factor: "price.momentum",
      extraFactors: [{ factor: "positions.interest_change" }],
      combination: "ic_weighted",
    },
  });
  const created = vi.fn();
  render(
    <NewModule
      hideTrigger
      folder="strategies"
      repository="/research"
      generated={{
        title: "从因子生成策略",
        name: "ic_strategy",
        build: name => factorStrategyTemplate(result, name),
      }}
      created={created}
    />,
  );
  fireEvent.click(screen.getByRole("button", { name: "创建" }));
  expect((await screen.findByRole("alert")).textContent).toContain(
    "暂不支持从 IC 加权合成报告生成策略，请先用等权合成重新评价",
  );
  act(() => setLocale("en-US"));
  expect(screen.getByRole("alert").textContent).toContain(
    "Strategy generation is not yet supported for IC-weighted composites.",
  );
  expect(lab).not.toHaveBeenCalled();
  expect(created).not.toHaveBeenCalled();
  act(() => setLocale("zh-CN"));
});

test("copying pre-fills and selects the module name, renames the target symbol, and Escape cancels", () => {
  expect(copyStem("CompositePortfolio", "strategies/composite.py")).toBe(
    "composite_portfolio_copy",
  );
  expect(
    copiedModule(
      "strategies",
      "class MultiFactorRank(Strategy):\n    pass\n\nclass CompositePortfolio(CombinedStrategy):\n    def components(self):\n        return [MultiFactorRank()]\n",
      "CompositePortfolio",
      "aaaaa",
    ),
  ).toEqual({
    id: "aaaaa.Aaaaa",
    content:
      "class _MultiFactorRank(Strategy):\n    pass\n\nclass Aaaaa(CombinedStrategy):\n    def components(self):\n        return [_MultiFactorRank()]\n",
  });
  expect(
    copiedModule(
      "factors",
      "def momentum(close):\n    return close\n\ndef reversal(close):\n    return -momentum(close)\n",
      "reversal",
      "my_rev",
    ),
  ).toEqual({
    id: "my_rev.my_rev",
    content:
      "def _momentum(close):\n    return close\n\ndef my_rev(close):\n    return -_momentum(close)\n",
  });
  const cancelCopy = vi.fn();
  render(
    <NewModule
      folder="strategies"
      repository="/research"
      copying={{ name: "composite_copy", content: "x = 1\n" }}
      cancelCopy={cancelCopy}
      created={vi.fn()}
    />,
  );
  const input = screen.getByLabelText("模块名") as HTMLInputElement;
  expect(input.value).toBe("composite_copy");
  expect(input.selectionStart).toBe(0);
  expect(input.selectionEnd).toBe("composite_copy".length);
  fireEvent.keyDown(input, { key: "Escape" });
  expect(cancelCopy).toHaveBeenCalledOnce();
});

test("modules sort by name, and a search covers descriptions and bar fields", () => {
  const items: ModuleItem[] = [
    {
      id: "price.momentum",
      name: "momentum",
      module: "price",
      description: "Return",
      inputs: ["close"],
    },
    {
      id: "carry.momentum",
      name: "momentum",
      module: "carry",
      description: "Term carry",
      inputs: ["close"],
    },
    {
      id: "price.reversal",
      name: "reversal",
      module: "price",
      description: "Negated return",
      inputs: ["close"],
    },
    {
      id: "positions.interest",
      name: "interest",
      module: "positions",
      description: "Open interest change",
      inputs: ["open_interest"],
    },
  ];
  const found = (search: string) => filterModuleItems(items, search).map(item => item.id);
  expect(found("")).toEqual([
    "positions.interest",
    "carry.momentum",
    "price.momentum",
    "price.reversal",
  ]);
  expect(found("negated")).toEqual(["price.reversal"]);
  expect(found("open_interest")).toEqual(["positions.interest"]);
});

test("edits made while saving remain an unsaved draft after navigation", async () => {
  let finish!: () => void;
  vi.mocked(lab).mockImplementation(
    () =>
      new Promise<void>(resolve => {
        finish = resolve;
      }) as never,
  );
  const store = createWorkspaceDrafts();
  const { result } = renderHook(() => useSource("/research"), {
    wrapper: ({ children }) => (
      <WorkspaceDraftScope store={store} namespace="research">
        {children}
      </WorkspaceDraftScope>
    ),
  });
  act(() => result.current.draft("strategies/a.py", "version_one = 1"));
  let saving!: Promise<string[]>;
  act(() => {
    saving = result.current.save();
  });
  act(() => result.current.draft("strategies/a.py", "version_two = 2"));
  await act(async () => {
    finish();
    await saving;
  });
  act(() => result.current.open("strategies/b.py"));
  expect(result.current.drafts["strategies/a.py"]).toBe("version_two = 2");
});

test("symbol field selects multiple symbols as tags, prioritizes stored products, expands months, warns on unstored symbols, and computes per-mode date placeholders", async () => {
  vi.mocked(lab).mockResolvedValue(
    create(StoredContractsSchema, {
      contracts: [
        {
          contract: "SHFE/rb/2026-10",
          daily: { firstDay: "2020-01-02", lastDay: "2026-02-13", days: 25 },
        },
        {
          contract: "GFEX/lc/2026-10",
          daily: { firstDay: "2023-07-21", lastDay: "2026-02-13", days: 25 },
        },
      ],
    }) as never,
  );
  const navigate = vi.fn();
  const context = {
    snapshot: { warehouse_endpoint: { host: "market", port: 7450, fingerprint: "f" } },
    navigate,
  } as unknown as TerminalContext;
  const onChange = vi.fn();
  const view = render(<SymbolField context={context} value="DCE/m" onChange={onChange} />);
  expect(await screen.findByText("未入库：DCE/m")).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "去采集" }));
  expect(navigate).toHaveBeenCalledWith("workspace.data-dataset", {
    params: { kind: "daily" },
  });

  // Opening the dropdown puts stored products first and allows picking multiple symbols & months.
  const combobox = screen.getByLabelText("品种");
  fireEvent.focus(combobox);
  const options = screen.getAllByRole("option");
  expect(options[0]!.textContent).toContain("SHFE/rb");
  expect(options[1]!.textContent).toContain("GFEX/lc");
  fireEvent.keyDown(combobox, { key: "ArrowDown" });
  fireEvent.keyDown(combobox, { key: "Enter" });
  expect(onChange).toHaveBeenLastCalledWith("DCE/m, GFEX/lc");
  fireEvent.click(options[0]!);
  expect(onChange).toHaveBeenLastCalledWith("DCE/m, SHFE/rb");

  fireEvent.click(screen.getByRole("button", { name: "SHFE/rb 月份合约" }));
  fireEvent.click(screen.getByRole("option", { name: /SHFE\/rb\/2026-10/ }));
  expect(onChange).toHaveBeenLastCalledWith("DCE/m, SHFE/rb/2026-10");

  view.rerender(<SymbolField context={context} value="SHFE/rb, DCE/i" onChange={onChange} />);
  fireEvent.click(screen.getByRole("button", { name: "移除 DCE/i" }));
  expect(onChange).toHaveBeenLastCalledWith("SHFE/rb");

  const { result } = renderHook(() => useStoredCatalog(context));
  await act(async () => {});
  expect(datePlaceholders(result.current, ["SHFE/rb"], true)).toEqual(["2020-01-02", "2026-02-13"]);
  expect(datePlaceholders(result.current, ["SHFE/rb", "GFEX/lc"], true)).toEqual([
    "各自最早",
    "2026-02-13",
  ]);
  expect(datePlaceholders(result.current, ["SHFE/rb", "GFEX/lc"], false)).toEqual([
    "2023-07-21",
    "2026-02-13",
  ]);

  const callsBefore = vi.mocked(lab).mock.calls.length;
  render(
    <SymbolField context={context} catalog={result.current} value="SHFE/rb" onChange={onChange} />,
  );
  expect(vi.mocked(lab).mock.calls.length).toBe(callsBefore);
});

test("shared universes expose built-in sector pools and sync custom pools across pickers", async () => {
  localStorage.clear();
  vi.mocked(lab).mockResolvedValue(
    create(StoredContractsSchema, {
      contracts: [
        {
          contract: "SHFE/rb/2026-10",
          daily: { firstDay: "2020-01-02", lastDay: "2026-02-13", days: 25 },
        },
        {
          contract: "DCE/i/2026-09",
          daily: { firstDay: "2020-01-02", lastDay: "2026-02-13", days: 25 },
        },
        {
          contract: "SHFE/cu/2026-10",
          daily: { firstDay: "2020-01-02", lastDay: "2026-02-13", days: 25 },
        },
      ],
    }) as never,
  );
  const context = {
    snapshot: { warehouse_endpoint: { host: "market", port: 7450, fingerprint: "f" } },
    navigate: vi.fn(),
  } as unknown as TerminalContext;
  const { result } = renderHook(() => useStoredCatalog(context));
  await act(async () => {});

  const onChange = vi.fn();
  render(
    <SymbolPopover
      context={context}
      catalog={result.current}
      value="SHFE/rb, DCE/i"
      fallbackLabel="螺纹钢、铁矿石"
      onChange={onChange}
    />,
  );

  // Built-in sector pool matches current symbols automatically.
  const trigger = screen.getByRole("button", { name: "快速选择品种" });
  expect(trigger.textContent).toBe("黑色 (2)");

  // Opening the popover displays built-in pools and allows saving a custom pool.
  fireEvent.click(trigger);
  fireEvent.click(screen.getByRole("button", { name: "全部已入库 (3)" }));
  expect(onChange).toHaveBeenLastCalledWith("SHFE/cu, SHFE/rb, DCE/i");

  fireEvent.click(screen.getByRole("button", { name: "存为品种池" }));
  fireEvent.change(screen.getByLabelText("品种池名称"), { target: { value: "双焦铁矿" } });
  fireEvent.click(screen.getByRole("button", { name: "保存" }));
  expect(readUniverses()).toEqual([{ name: "双焦铁矿", symbols: "SHFE/rb, DCE/i" }]);
  expect(trigger.textContent).toBe("双焦铁矿 (2)");

  // Deleting the custom pool reverts the label to the built-in sector pool.
  fireEvent.click(screen.getByRole("button", { name: "删除品种池 双焦铁矿" }));
  expect(readUniverses()).toEqual([]);
  expect(trigger.textContent).toBe("黑色 (2)");
});
