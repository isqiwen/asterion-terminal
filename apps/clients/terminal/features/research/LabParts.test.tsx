import { act, fireEvent, render, renderHook, screen } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { lab } from "../../src/bridge/lab";
import type { TerminalContext } from "../../src/host/feature-registry";
import { StoredContractsSchema } from "../../src/generated/asterion/lab_pb";
import { feature } from "./feature";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import {
  datePlaceholders,
  filterModuleItems,
  NewModule,
  SymbolField,
  useSource,
  useStoredCatalog,
  type ModuleItem,
} from "./LabParts";
import { copiedModule, copyStem } from "./research-lab";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
registerTerminalFeatures([feature]);

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
  expect(navigate).toHaveBeenCalledWith("workspace.data-dataset");

  // Opening the dropdown puts stored products first and allows picking multiple symbols & months.
  const combobox = screen.getByLabelText("品种");
  fireEvent.focus(combobox);
  const options = screen.getAllByRole("option");
  expect(options[0]!.textContent).toContain("SHFE/rb");
  expect(options[1]!.textContent).toContain("GFEX/lc");
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
});
