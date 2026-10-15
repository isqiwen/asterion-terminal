import { render, screen } from "@testing-library/react";
import { expect, test } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { plugin } from "./plugin";
import { RefreshObservation } from "./RefreshObservation";
import type { TableRefreshObservation } from "../../src/bridge/client";
registerTerminalPlugins([plugin]);
const observation: TableRefreshObservation = {
  kind: "explicit_refresh_response_observation",
  version: 1,
  id: "a".repeat(64),
  operation: "b".repeat(64),
  data_instance: "data",
  task_instance: "tasks",
  task_id: "refresh-one",
  attempt: 1,
  provider: "fixture",
  provider_artifact: "c".repeat(64),
  selected_snapshot: "d".repeat(64),
  selected_content: "e".repeat(64),
  normalized_object: "f".repeat(64),
  format: "tushare.fut_daily.raw.v1",
  body: '{ "code":0 }\n',
  response_sha256: "1".repeat(64),
  response_bytes: 13,
  received_at_ns: "100",
  recorded_at_ns: "200",
  unchanged: true,
  content_changed_from_selected: false,
  revision: {
    sequence: 1,
    table: "futures.daily",
    schema: 1,
    source: "tushare.fut_daily.v3",
    exchange: "SHFE",
    period: "2025-07-25",
    content: "e".repeat(64),
    rows: 1,
    bytes: 20,
    acquired_at_ns: "1",
    provider_artifact: "2".repeat(64),
    snapshot: "d".repeat(64),
  },
};
test("post-fetch same-content evidence is visibly distinct from original import", () => {
  render(<RefreshObservation observation={observation} />);
  expect(document.body.textContent).toContain("不是旧修订首次导入原文");
  expect(document.body.textContent).toContain("规范化内容与预览所选修订相同");
  expect(document.body.textContent).toContain("本次响应接收时间 100 ns");
  expect(document.body.textContent).toContain("观测落盘时间 200 ns");
  expect(screen.getByLabelText("独立观测响应原文").textContent).toBe(observation.body);
  expect(screen.queryByRole("button")).toBeNull();
});
test("changed content names selected and corresponding revisions separately", () => {
  render(
    <RefreshObservation
      observation={{
        ...observation,
        content_changed_from_selected: true,
        unchanged: false,
        revision: { ...observation.revision, snapshot: "3".repeat(64) },
      }}
    />,
  );
  expect(document.body.textContent).toContain("规范化内容不同于预览所选修订");
  expect(document.body.textContent).toContain("d".repeat(64));
  expect(document.body.textContent).toContain("3".repeat(64));
});
test("absence never offers an automatic fetch or reconstructed observation", () => {
  render(<RefreshObservation observation={null} />);
  expect(document.body.textContent).toContain("尚无独立重取观测");
  expect(screen.queryByLabelText("独立观测响应原文")).toBeNull();
});
