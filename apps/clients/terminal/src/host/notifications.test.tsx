import { openPopover } from "../testing/popover";
import { act, fireEvent, render, screen } from "@testing-library/react";
import { expect, test } from "vitest";
import { BackendError } from "../i18n/errors";
import { NotificationCenter, notify } from "./notifications";

test("the status bar shows the latest unread message and collapses to 通知 with its count once opened", () => {
  render(<NotificationCenter />);
  act(() => {
    notify(new BackendError("invalid_request", "bad input"));
    notify(new BackendError("conflict", "order refused"), {
      namespace: "asterion.terminal.trading",
    });
  });
  const latest = screen.getByRole("button", { expanded: false });
  expect(latest.textContent).toBe("操作与当前状态冲突: order refused2");
  // An unread trading error stays highlighted until the list is opened.
  expect(latest.className).toContain("urgent");
  openPopover(latest);
  const panel = screen.getByRole("region", { name: "通知" });
  const items = panel.querySelectorAll("li");
  expect([...items].map(item => item.querySelector(".notification-text")!.textContent)).toEqual([
    "操作与当前状态冲突: order refused",
    "请求无效，请检查输入: bad input",
  ]);
  const trigger = screen.getByRole("button", { expanded: true });
  expect(trigger.textContent).toBe("通知2");
  expect(trigger.className).toBe("notification-kept error");
  fireEvent(panel, Object.assign(new Event("toggle"), { newState: "closed" }));
  expect(screen.getByRole("button", { expanded: false }).textContent).toBe("通知2");
  act(() => {
    notify(new BackendError("unavailable", "reconnecting"));
  });
  expect(screen.getByRole("button", { expanded: false }).textContent).toBe(
    "服务暂不可用: reconnecting1",
  );
  openPopover(screen.getByRole("button", { expanded: false }));
  fireEvent.click(screen.getByRole("button", { name: "清空" }));
  expect(screen.getByText("没有消息")).toBeDefined();
});

test("read messages keep 通知 yellow for warnings and red for errors until cleared", () => {
  render(<NotificationCenter />);
  act(() => {
    notify("disk is low", { severity: "warning" });
  });
  openPopover(screen.getByRole("button", { expanded: false }));
  const trigger = screen.getByRole("button", { expanded: true });
  expect(trigger.className).toBe("notification-kept warning");
  expect(trigger.textContent).toBe("通知1");
  act(() => {
    notify(new BackendError("unavailable", "Lab stopped"));
  });
  expect(trigger.className).toBe("notification-kept error");
  expect(trigger.textContent).toBe("通知1");
  fireEvent.click(screen.getByRole("button", { name: "清空" }));
  expect(trigger.className).toBe("notification-none");
  expect(trigger.textContent).toBe("通知");
});

test("the same message repeated in a row is one entry with its count", () => {
  render(<NotificationCenter />);
  act(() => {
    for (let i = 0; i < 3; i++) notify(new BackendError("unavailable", "Lab restarted"));
  });
  const latest = screen.getByRole("button", { expanded: false });
  expect(latest.querySelector(".notification-count")!.textContent).toBe("1");
  openPopover(latest);
  const items = screen.getByRole("region", { name: "通知" }).querySelectorAll("li");
  expect(items).toHaveLength(1);
  expect(items[0]!.querySelector(".notification-text")!.textContent).toContain("×3");
});
