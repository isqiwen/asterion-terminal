import { create } from "@bufbuild/protobuf";
import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { beforeEach, expect, test, vi } from "vitest";
import { lab } from "../bridge/lab";
import { LabActivitySchema, RunState, TaskKind } from "../generated/asterion/lab_pb";
import { openPopover } from "../testing/popover";
import { LabHeaderActivity, LabStatusTask } from "./components/LabActivityPopover";
import { LabActivityProvider, useLabActivity } from "./lab-activity";
import { NotificationCenter } from "./notifications";

vi.mock("../bridge/lab", () => ({
  lab: vi.fn(),
  onLabMutation: vi.fn(() => () => {}),
}));

beforeEach(() => {
  vi.mocked(lab).mockReset();
});

test("status bar task pill shows active sweep progress, opens popover with controls, and guards window unload", async () => {
  const startedMs = BigInt(Date.now() - 20_000);
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation.case === "getActivity") {
      return create(LabActivitySchema, {
        runningProcesses: 4,
        maxProcesses: 7,
        queuedRuns: 10,
        active: [
          {
            id: "sw-1",
            kind: TaskKind.SWEEP,
            strategy: "ma_cross.MaCross",
            symbols: ["SHFE/rb"],
            total: 20,
            done: 10,
            succeeded: 10,
            failed: 0,
            running: 4,
            queued: 6,
            startedMs,
            state: RunState.RUNNING,
          },
        ],
        recent: [],
      }) as never;
    }
    return {} as never;
  });

  const openSweep = vi.fn();
  render(
    <LabActivityProvider
      ready={true}
      currentModule="market"
      openRun={vi.fn()}
      openSweep={openSweep}
    >
      <LabStatusTask openRun={vi.fn()} openSweep={openSweep} openFactors={vi.fn()} />
      <LabHeaderActivity openRun={vi.fn()} openSweep={openSweep} openFactors={vi.fn()} />
    </LabActivityProvider>,
  );

  const pill = await screen.findByRole("button", { name: /扫描 MaCross 10\/20/ });
  expect(pill.textContent).toContain("50%");
  expect(pill.title).toContain("运行 4 / 7 核");
  expect(pill.title).toContain("排队 10");

  // Window close is guarded while a task is active.
  const unloadEvent = new Event("beforeunload", { cancelable: true });
  window.dispatchEvent(unloadEvent);
  expect(unloadEvent.defaultPrevented).toBe(true);

  openPopover(pill);
  const panel = screen.getByRole("region", { name: "研究任务" });
  expect(panel.textContent).toContain("进行中");
  fireEvent.click(screen.getByRole("button", { name: "查看" }));
  expect(openSweep).toHaveBeenCalledWith("sw-1");

  openPopover(pill);
  fireEvent.click(screen.getByRole("button", { name: "取消" }));
  await waitFor(() =>
    expect(vi.mocked(lab)).toHaveBeenCalledWith({
      case: "cancelSweep",
      value: { id: "sw-1" },
    }),
  );
});

test("finishing an active task while outside research notifies the notification center with click-through", async () => {
  const startedMs = BigInt(Date.now() - 5_000);
  let finished = false;
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation.case === "getActivity") {
      return create(
        LabActivitySchema,
        finished
          ? {
              runningProcesses: 0,
              maxProcesses: 4,
              queuedRuns: 0,
              active: [],
              recent: [
                {
                  id: "sw-done",
                  kind: TaskKind.SWEEP,
                  strategy: "ma_cross.MaCross",
                  total: 4,
                  done: 4,
                  succeeded: 4,
                  failed: 0,
                  startedMs,
                  finishedMs: BigInt(Date.now()),
                  state: RunState.SUCCEEDED,
                },
              ],
            }
          : {
              runningProcesses: 2,
              maxProcesses: 4,
              queuedRuns: 2,
              active: [
                {
                  id: "sw-done",
                  kind: TaskKind.SWEEP,
                  strategy: "ma_cross.MaCross",
                  total: 4,
                  done: 1,
                  succeeded: 1,
                  failed: 0,
                  running: 2,
                  queued: 1,
                  startedMs,
                  state: RunState.RUNNING,
                },
              ],
              recent: [],
            },
      ) as never;
    }
    return {} as never;
  });

  function Refresher() {
    const activity = useLabActivity();
    return (
      <button type="button" onClick={() => void activity?.refresh()}>
        刷新活动
      </button>
    );
  }

  const openSweep = vi.fn();
  render(
    <LabActivityProvider
      ready={true}
      currentModule="market"
      openRun={vi.fn()}
      openSweep={openSweep}
    >
      <Refresher />
      <NotificationCenter />
    </LabActivityProvider>,
  );

  await waitFor(() => expect(lab).toHaveBeenCalled());
  finished = true;
  await act(async () => {
    fireEvent.click(screen.getByRole("button", { name: "刷新活动" }));
  });

  const noticeButton = await screen.findByRole("button", { name: /扫描 MaCross 已完成（4 组）/ });
  openPopover(noticeButton);
  fireEvent.click(screen.getByRole("button", { name: "扫描 MaCross 已完成（4 组）" }));
  expect(openSweep).toHaveBeenCalledWith("sw-done");

  // Once idle, beforeunload is no longer prevented.
  const unloadEvent = new Event("beforeunload", { cancelable: true });
  window.dispatchEvent(unloadEvent);
  expect(unloadEvent.defaultPrevented).toBe(false);
});

test("factor tasks cancel via cancelFactor and click-through opens the completed factor report", async () => {
  const startedMs = BigInt(Date.now() - 4_000);
  let finished = false;
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation.case === "getActivity") {
      return create(
        LabActivitySchema,
        finished
          ? {
              runningProcesses: 0,
              maxProcesses: 4,
              queuedRuns: 0,
              active: [],
              recent: [
                {
                  id: "mom-1",
                  kind: TaskKind.FACTOR,
                  strategy: "price.momentum",
                  total: 1,
                  done: 1,
                  succeeded: 1,
                  failed: 0,
                  startedMs,
                  finishedMs: BigInt(Date.now()),
                  state: RunState.SUCCEEDED,
                },
              ],
            }
          : {
              runningProcesses: 1,
              maxProcesses: 4,
              queuedRuns: 0,
              active: [
                {
                  id: "mom-1",
                  kind: TaskKind.FACTOR,
                  strategy: "price.momentum",
                  total: 1,
                  done: 0,
                  running: 1,
                  startedMs,
                  state: RunState.RUNNING,
                },
              ],
              recent: [],
            },
      ) as never;
    }
    return {} as never;
  });

  function Refresher() {
    const activity = useLabActivity();
    return (
      <button type="button" onClick={() => void activity?.refresh()}>
        刷新活动
      </button>
    );
  }

  const openFactors = vi.fn();
  render(
    <LabActivityProvider
      ready={true}
      currentModule="market"
      openRun={vi.fn()}
      openSweep={vi.fn()}
      openFactors={openFactors}
    >
      <LabStatusTask openRun={vi.fn()} openSweep={vi.fn()} openFactors={openFactors} />
      <Refresher />
      <NotificationCenter />
    </LabActivityProvider>,
  );

  const pill = await screen.findByRole("button", { name: /评价 momentum/ });
  openPopover(pill);
  fireEvent.click(screen.getByRole("button", { name: "取消" }));
  await waitFor(() =>
    expect(vi.mocked(lab)).toHaveBeenCalledWith({
      case: "cancelFactor",
      value: { id: "mom-1" },
    }),
  );

  finished = true;
  await act(async () => {
    fireEvent.click(screen.getByRole("button", { name: "刷新活动" }));
  });

  const notice = await screen.findByRole("button", { name: /评价 momentum 已完成/ });
  openPopover(notice);
  fireEvent.click(screen.getByRole("button", { name: "评价 momentum 已完成" }));
  expect(openFactors).toHaveBeenCalledWith("mom-1");
});
