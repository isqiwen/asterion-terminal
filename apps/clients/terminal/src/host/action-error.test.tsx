import { openPopover } from "../testing/popover";
import { render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import { BackendError } from "../i18n/errors";
import { ActionError } from "./action-error";
import { NotificationCenter, useErrorNotification } from "./notifications";

function PageRead({ error }: { error: BackendError }) {
  useErrorNotification(error, "asterion.terminal.data-workbench");
  return null;
}

test("page read and protocol failures go to notifications while input errors stay beside the form", () => {
  const show = (error: BackendError) => (
    <>
      <main>
        <ActionError error={new BackendError("invalid_request", "invalid decimal: abc")} />
        <ActionError
          error={new BackendError("conflict", "warehouse protocol differs from this Terminal")}
          namespace="asterion.terminal.data-workbench"
        />
        <PageRead error={error} />
      </main>
      <NotificationCenter />
    </>
  );
  const view = render(show(new BackendError("invalid_request", "unknown Protobuf field")));
  expect(screen.getByRole("main").textContent).not.toContain("通信协议");
  expect(within(screen.getByRole("alert")).getByTitle(/invalid decimal: abc/)).toBeTruthy();
  openPopover(screen.getByRole("button", { expanded: false }));
  const panel = screen.getByRole("region", { name: "通知" });
  expect(panel.textContent).toContain("数据");
  expect(panel.textContent).toContain("通信协议不一致");
  expect(panel.querySelectorAll("li")).toHaveLength(2);
  view.rerender(show(new BackendError("invalid_request", "unknown Protobuf field")));
  expect(panel.querySelectorAll("li")).toHaveLength(2);
  expect(panel.querySelector("ol")!.textContent).not.toContain("×");

  view.rerender(show(new BackendError("permission_denied", "this client is not registered")));
  expect(screen.getByRole("main").textContent).not.toContain("registered");
  expect(panel.querySelectorAll("li")).toHaveLength(3);
});
