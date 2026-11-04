import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { LocalLogsButton } from "./LocalLogsButton";
import { NotificationCenter } from "../notifications";

const { openLogs } = vi.hoisted(() => ({ openLogs: vi.fn() }));
vi.mock("../../bridge/desktop", () => ({
  nativeDesktop: true,
  desktop: () => ({ openLogs }),
}));

test("opens the native-owned logs directory and reports an OS open failure", async () => {
  openLogs.mockResolvedValueOnce(undefined).mockRejectedValueOnce(new Error("Permission denied"));
  render(
    <>
      <LocalLogsButton />
      <NotificationCenter />
    </>,
  );
  const button = screen.getByRole("button", { name: "打开本机日志目录" });
  fireEvent.click(button);
  await waitFor(() => expect(openLogs).toHaveBeenCalledWith());
  expect(screen.queryByText(/Permission denied/)).toBeNull();
  fireEvent.click(button);
  expect(await screen.findByText(/Permission denied/)).toBeDefined();
});
