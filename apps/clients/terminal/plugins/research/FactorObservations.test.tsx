import { fireEvent, render, screen, within } from "@testing-library/react";
import { test, expect } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { FactorObservations } from "./FactorObservations";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);

test("known zero remains numeric while feature warm-up and the unknown future label are distinct", () => {
  render(
    <FactorObservations
      kind="tables"
      rows={[
        {
          role: "full_sample",
          label_crosses_boundary: false,
          event_index: 0,
          observed: "2026-09-21",
          value: null,
          forward_return: 0,
          label_end_index: 1,
          label: "2026-09-22",
          evaluation_warmup: true,
          included: false,
        },
        {
          role: "full_sample",
          label_crosses_boundary: false,
          event_index: 1,
          observed: "2026-09-22",
          value: 0,
          forward_return: 0,
          label_end_index: 2,
          label: "2026-09-23",
          evaluation_warmup: false,
          included: true,
        },
        {
          role: "full_sample",
          label_crosses_boundary: false,
          event_index: 2,
          observed: "2026-09-23",
          value: 0,
          forward_return: null,
          label_end_index: null,
          label: null,
          evaluation_warmup: false,
          included: false,
        },
      ]}
    />,
  );
  fireEvent.click(screen.getByText("逐行特征与标签证据", { exact: true }));
  const rows = within(screen.getByRole("table", { name: "逐行特征与标签证据" })).getAllByRole(
    "row",
  );
  expect(within(rows[1]).getAllByRole("cell")[2].textContent).toBe("特征尚未定义");
  expect(within(rows[1]).getAllByRole("cell")[5].textContent).toBe("评价预热未完成");
  expect(within(rows[2]).getAllByRole("cell")[2].textContent).toBe("0");
  expect(within(rows[2]).getAllByRole("cell")[4].textContent).toBe("0");
  expect(within(rows[2]).getAllByRole("cell")[5].textContent).toBe("参与评价");
  expect(within(rows[3]).getAllByRole("cell")[2].textContent).toBe("0");
  expect(within(rows[3]).getAllByRole("cell")[4].textContent).toBe("未来标签尚未知");
});

test("known boundary-excluded labels retain their value and explicit development role", () => {
  render(
    <FactorObservations
      kind="tables"
      rows={[
        {
          event_index: 48,
          observed: "2026-09-21",
          value: 0,
          forward_return: 0.1,
          label_end_index: 50,
          label: "2026-09-23",
          evaluation_warmup: false,
          included: false,
          role: "development",
          label_crosses_boundary: true,
        },
      ]}
    />,
  );
  fireEvent.click(screen.getByText("逐行特征与标签证据", { exact: true }));
  const row = within(screen.getByRole("table", { name: "逐行特征与标签证据" })).getAllByRole(
    "row",
  )[1];
  const cells = within(row).getAllByRole("cell");
  expect(cells[2].textContent).toBe("0");
  expect(cells[4].textContent).toBe("0.1");
  expect(cells[5].textContent).toBe("标签跨开发边界，排除评价");
  expect(cells[6].textContent).toBe("开发段");
});

test("saved expression units and undefined reasons remain distinct from numeric zero and unknown future labels", () => {
  const common = {
    role: "full_sample" as const,
    label_crosses_boundary: false,
    observed: "2026-09-21",
    label: "2026-09-22",
    label_end_index: 4,
    forward_return: 0,
    evaluation_warmup: false,
    included: false,
  };
  render(
    <FactorObservations
      kind="tables"
      pricePower={1}
      rows={[
        { ...common, event_index: 0, value: null, feature_state: "FEATURE_ZERO_DENOMINATOR" },
        { ...common, event_index: 1, value: null, feature_state: "FEATURE_NON_FINITE" },
        { ...common, event_index: 2, value: 0, feature_state: "FEATURE_VALUE", included: true },
      ]}
    />,
  );
  expect(screen.getByRole("columnheader", { name: "特征值（价格单位）" })).toBeTruthy();
  expect(screen.getByText("分母为零，特征无效")).toBeTruthy();
  expect(screen.getByText("非有限结果，特征无效")).toBeTruthy();
  expect(screen.getAllByText("特征无效，排除评价")).toHaveLength(2);
  const rows = screen.getAllByRole("row");
  expect(within(rows[3]).getAllByRole("cell")[2].textContent).toBe("0");
  expect(within(rows[3]).getByText("参与评价")).toBeTruthy();
  expect(screen.queryByText("未来标签尚未知")).toBeNull();
});
