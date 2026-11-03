import { act, fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { BackendError } from "../i18n/errors";
import { Notifications, notify } from "./notifications";

test("failures close after 8 s, trading ones stay, and a missing Market is no failure", () => {
  vi.useFakeTimers();
  render(<Notifications />);
  act(() => {
    notify(new BackendError("market_unconfigured", "no Market is configured"));
    notify(new BackendError("invalid_request", "bad input"));
    notify(new BackendError("conflict", "order refused"), { sticky: true });
  });
  expect(screen.getAllByRole("alert").map(item => item.textContent)).toEqual([
    "请求无效，请检查输入复制诊断×",
    "操作与当前状态冲突复制诊断×",
  ]);
  act(() => vi.advanceTimersByTime(8000));
  expect(screen.getAllByRole("alert")).toHaveLength(1);
  act(() => fireEvent.click(screen.getByRole("button", { name: "关闭" })));
  expect(screen.queryByRole("alert")).toBeNull();
  vi.useRealTimers();
});
