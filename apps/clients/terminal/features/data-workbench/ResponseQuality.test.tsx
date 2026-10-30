import { render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { feature } from "./feature";
import { ResponseQuality } from "./ResponseQuality";
import type { TablePage } from "../../src/bridge/client";
registerTerminalFeatures([feature]);
const base: NonNullable<TablePage["response_review"]> = {
  status: "consistent",
  reason: "checked_recorded_partition",
  response_fields: 14,
  response_rows: 2,
  normalized_rows: 1,
  dated_rows: 1,
  matched_rows: 1,
  compared_cells: 12,
  skipped_keys: ["CU.SHF"],
  unknown_keys: [],
  differences: [],
};
test("consistent review states counts, exclusions and exact field rules", () => {
  render(<ResponseQuality review={base} />);
  expect(screen.getByRole("status", { hidden: true }).textContent).toBe("已核对：记录分区一致");
  expect(document.body.textContent).toContain("响应 2 行");
  expect(document.body.textContent).toContain("CU.SHF");
  const rules = screen.getByRole("table", { hidden: true });
  expect(within(rules).getAllByRole("row", { hidden: true })).toHaveLength(15);
  expect(rules.textContent).toContain("原值乘10000；2位小数");
});
test("differences expose original, exact expected and actual values with precision reason", () => {
  render(
    <ResponseQuality
      review={{
        ...base,
        status: "differences",
        differences: [
          {
            kind: "value_mismatch",
            key: "CU2403.SHF",
            field: "amount",
            raw: "43200000.000026",
            expected: "432000000000.26",
            actual: "432000000000.25",
          },
          {
            kind: "not_exactly_representable",
            key: "CU2403.SHF",
            field: "close",
            raw: "1.000000001",
            expected: "1.000000001",
            actual: "1",
          },
        ],
      }}
    />,
  );
  const table = screen.getByRole("table", { name: "响应分区差异", hidden: true });
  expect(table.textContent).toContain("43200000.000026");
  expect(table.textContent).toContain("432000000000.26");
  expect(table.textContent).toContain("432000000000.25");
  expect(table.textContent).toContain("超出目标精度或范围，禁止舍入");
});
test("missing response remains unknown rather than an empty passing response", () => {
  render(
    <ResponseQuality
      review={{
        ...base,
        status: "unknown",
        reason: "raw_missing",
        response_fields: 0,
        response_rows: 0,
        compared_cells: 0,
      }}
    />,
  );
  expect(screen.getByRole("status", { hidden: true }).textContent).toBe("证据不足：未知");
  expect(document.body.textContent).toContain("响应字段、行数与对应关系未知");
  expect(document.body.textContent).not.toContain("响应 0 行");
});
