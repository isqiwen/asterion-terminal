import { act, fireEvent, render, screen } from "@testing-library/react";
import { expect, test } from "vitest";
import { BackendError } from "../i18n/errors";
import { NotificationCenter, notify } from "./notifications";

test("the status bar shows the latest message; opened, all of them until cleared", () => {
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
  fireEvent.click(latest);
  const items = screen.getByRole("region", { name: "通知" }).querySelectorAll("li");
  expect([...items].map(item => item.querySelector(".notification-text")!.textContent)).toEqual([
    "操作与当前状态冲突: order refused",
    "请求无效，请检查输入: bad input",
  ]);
  expect(screen.getByRole("button", { expanded: true }).className).not.toContain("urgent");
  fireEvent.click(screen.getByRole("button", { name: "清空" }));
  expect(screen.getByText("没有消息")).toBeDefined();
});

test("the same message repeated in a row is one entry with its count", () => {
  render(<NotificationCenter />);
  act(() => {
    for (let i = 0; i < 3; i++) notify(new BackendError("unavailable", "Lab restarted"));
  });
  const latest = screen.getByRole("button", { expanded: false });
  expect(latest.querySelector(".notification-count")!.textContent).toBe("1");
  fireEvent.click(latest);
  const items = screen.getByRole("region", { name: "通知" }).querySelectorAll("li");
  expect(items).toHaveLength(1);
  expect(items[0]!.querySelector(".notification-text")!.textContent).toContain("×3");
});
