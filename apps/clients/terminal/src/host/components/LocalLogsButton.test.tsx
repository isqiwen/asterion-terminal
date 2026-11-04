import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { NotificationCenter } from "../notifications";

const { openLogs } = vi.hoisted(() => ({ openLogs: vi.fn() }));
vi.mock("../../bridge/desktop", () => ({
  nativeDesktop: true,
  desktop: () => ({ openLogs }),
}));

test("the notification centre opens the logs directory even with no messages", async () => {
  openLogs.mockResolvedValueOnce(undefined).mockRejectedValueOnce(new Error("Permission denied"));
  render(<NotificationCenter />);
  fireEvent.click(screen.getByRole("button", { name: "通知" }));
  expect(screen.getByText("没有消息")).toBeDefined();
  const button = screen.getByRole("button", { name: "打开本机日志目录" });
  fireEvent.click(button);
  await waitFor(() => expect(openLogs).toHaveBeenCalledWith());
  expect(screen.queryByText(/Permission denied/)).toBeNull();
  fireEvent.click(button);
  expect(await screen.findByText(/Permission denied/)).toBeDefined();
});
