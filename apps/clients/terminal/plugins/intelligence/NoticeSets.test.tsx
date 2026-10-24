import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { test, expect, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import type {
  NoticeObject,
  NoticeRecord,
  NoticeSetRequest,
  NoticeSetView,
} from "../../src/bridge/client";
import { NoticeSets } from "./NoticeSets";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const event: NoticeObject = {
  node: "stable",
  service: "intel",
  domain: "intelligence",
  kind: "intelligence_event",
  id: "event-one",
  version: "a".repeat(64),
};
test("failed set save retains the exact preview request and retry confirms the original operation", async () => {
  const snapshot = readySnapshot();
  snapshot.data = null; // Set location comes from its fixed member content, not a paired connection.
  snapshot.intelligence_service = {
    node_id: "local",
    owner_node_id: "stable",
    service: "intel",
    connection_id: "intel-connection",
    remote: false,
    host: "",
    port: 0,
    records: [],
  };
  let preview: NoticeSetView;
  const query = vi.fn(async (method: string, params?: unknown) => {
    if (method === "intelligence.list")
      return {
        ...snapshot,
        intelligence_records: [
          {
            event: { reference: event },
            published: {
              reference: { ...event, kind: "intelligence_publication" },
              content: { ...event, domain: "data", kind: "intelligence_content" },
            },
            contract: "SHFE/rb/2026-11",
          } as NoticeRecord,
        ],
      };
    if (method === "intelligence.set.preview") {
      const request = (params as { request: NoticeSetRequest }).request;
      preview = {
        state: "preview",
        request: { ...request, program_artifact: "b".repeat(64) },
        observed_at_ms: "1790000000001",
        reference: null,
        available_at_ms: null,
        members: [
          {
            event: {
              event: {
                reference: event,
                initial_publication: { ...event, kind: "intelligence_publication" },
                contract: "SHFE/rb/2026-11",
                use_scope: "research",
                rule_id: "contract_margin_announcement",
                available_at_ms: "1790000000000",
              },
              head: { ...event, kind: "intelligence_publication" },
              selected_publication: { ...event, kind: "intelligence_publication" },
              withdrawn: false,
              latest_change: null,
              pending_change: null,
            },
            publication: { ...event, kind: "intelligence_publication" },
            content: { ...event, domain: "data", kind: "intelligence_content" },
            limitations: [],
          },
        ],
        exclusions: [],
      };
      return { ...snapshot, notice_set: preview };
    }
    return {
      ...snapshot,
      notice_set: {
        ...preview!,
        state: "published" as const,
        reference: { ...event, kind: "intelligence_set" },
        available_at_ms: "1790000000002",
      },
    };
  });
  const trade = vi.fn().mockRejectedValueOnce(Error("reply lost")).mockResolvedValue(undefined);
  const context = {
    snapshot,
    busy: false,
    error: "",
    query,
    trade,
    navigate: vi.fn(),
    openSettings: vi.fn(),
  };
  render(<NoticeSets context={context} />);
  fireEvent.click(screen.getByRole("button", { name: "读取前32条候选" }));
  fireEvent.click(await screen.findByRole("checkbox"));
  fireEvent.change(screen.getByLabelText("集合操作 ID"), { target: { value: "original-set" } });
  fireEvent.change(screen.getByLabelText("集合名称"), { target: { value: "Explicit test set" } });
  fireEvent.change(screen.getByLabelText("集合截止时间（UTC）"), {
    target: { value: "2026-01-01T00:00:00.000" },
  });
  fireEvent.click(screen.getByRole("button", { name: "预览固定选择与排除" }));
  const save = await screen.findByRole("button", { name: "确认保存并发布集合" });
  fireEvent.click(save);
  await waitFor(() => expect(trade).toHaveBeenCalledTimes(1));
  const retry = await screen.findByRole("button", { name: "重试原集合保存" });
  await waitFor(() => expect((retry as HTMLButtonElement).disabled).toBe(false));
  expect((screen.getByLabelText("集合名称") as HTMLInputElement).disabled).toBe(true);
  fireEvent.click(retry);
  await waitFor(() => expect(trade).toHaveBeenCalledTimes(2));
  expect(trade.mock.calls[0]).toEqual(trade.mock.calls[1]);
  expect(trade.mock.calls[0][1].request.program_artifact).toBe("b".repeat(64));
  expect(query).toHaveBeenCalledWith("intelligence.set.get", { id: "original-set" });
  expect(
    ((await screen.findByRole("button", { name: "交给 Research 集合审阅" })) as HTMLButtonElement)
      .disabled,
  ).toBe(false);
});
