import { render, screen } from "@testing-library/react";
import { expect, test } from "vitest";
import { setLocale } from "./index";
import { BackendError, ErrorNotice } from "./errors";

test("protocol mismatch names the service and explains how to recover in both languages", () => {
  const { rerender } = render(
    <ErrorNotice
      error={new BackendError("conflict", "market protocol differs from this Terminal")}
    />,
  );
  expect(screen.getByRole("button").textContent).toBe(
    "行情服务与 Terminal 的通信协议不一致，请将两边更新到同一版本。",
  );
  setLocale("en-US");
  rerender(
    <ErrorNotice
      error={new BackendError("conflict", "market protocol differs from this Terminal")}
    />,
  );
  expect(screen.getByRole("button").textContent).toBe(
    "Market and Terminal use different communication protocols. Update both to the same release.",
  );
});

test("an untranslated failure keeps its cause visible without hovering", () => {
  render(<ErrorNotice error={new BackendError("conflict", "Requested feed is already in use")} />);
  expect(screen.getByRole("button").textContent).toBe(
    "操作与当前状态冲突: Requested feed is already in use",
  );
});
