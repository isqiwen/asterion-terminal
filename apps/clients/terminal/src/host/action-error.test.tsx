import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import { BackendError } from "../i18n/errors";
import { ActionError, RequestError } from "./action-error";
import { NotificationCenter } from "./notifications";

test("protocol failures from page requests go to notifications while input errors stay beside the form", () => {
  const show = (error: BackendError) => (
    <>
      <main>
        <ActionError error={new BackendError("invalid_request", "invalid decimal: abc")} />
        <RequestError error={error} namespace="asterion.terminal.data-workbench" />
      </main>
      <NotificationCenter />
    </>
  );
  const view = render(show(new BackendError("invalid_request", "unknown Protobuf field")));
  expect(screen.getByRole("main").textContent).not.toContain("通信协议");
  expect(within(screen.getByRole("alert")).getByTitle(/invalid decimal: abc/)).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { expanded: false }));
  const panel = screen.getByRole("region", { name: "通知" });
  expect(panel.textContent).toContain("数据");
  expect(panel.textContent).toContain("通信协议不一致");
  view.rerender(show(new BackendError("invalid_request", "unknown Protobuf field")));
  expect(panel.querySelectorAll("li")).toHaveLength(1);
  expect(panel.querySelector("ol")!.textContent).not.toContain("×");
});
