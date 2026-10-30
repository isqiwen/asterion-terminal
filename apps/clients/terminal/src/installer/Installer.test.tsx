import { render, screen, fireEvent, waitFor, within } from "@testing-library/react";
import { test, expect, vi } from "vitest";
import { Installer, type Installation, type InstallerBridge } from "./Installer";
const empty: Installation = {
  directory: "/applications/terminal",
  root: "/data/node",
  installed: false,
  version: "",
  candidate_version: "0.1.0",
  source_available: true,
  deployment_action: "install",
  busy: [],
  conflict: "",
  pending: "",
  preserve_data: true,
};
const installed: Installation = {
  ...empty,
  installed: true,
  version: "0.1.0",
  deployment_action: "repair",
};
function bridge(state: Installation = empty): InstallerBridge {
  return {
    inspect: vi.fn().mockResolvedValue(state),
    run: vi.fn().mockResolvedValue(installed),
    open: vi.fn().mockResolvedValue(undefined),
    onProgress: () => () => {},
  };
}
function operations() {
  return within(screen.getByRole("navigation"));
}
function execute(name: string) {
  fireEvent.click(screen.getByRole("checkbox", { name: /我已确认操作范围/ }));
  fireEvent.click(screen.getByRole("button", { name }));
}
test("first installation requires confirmation then switches to repair and uninstall", async () => {
  const api = bridge();
  render(<Installer bridge={api} />);
  await screen.findByText("/applications/terminal");
  expect(operations().getAllByRole("button")).toHaveLength(1);
  expect(operations().queryByRole("button", { name: /升级|卸载|修复/ })).toBeNull();
  expect(
    (screen.getByRole("button", { name: "确认执行 · 安装" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  expect(api.run).not.toHaveBeenCalled();
  execute("确认执行 · 安装");
  await screen.findByText("操作已完成。用户资料已保留。");
  expect(api.run).toHaveBeenCalledWith("install", false);
  expect(screen.getByRole("heading", { level: 1 }).textContent).toBe("维护 Terminal");
  expect(operations().getAllByRole("button")).toHaveLength(2);
  expect(operations().queryByRole("button", { name: /安装|升级/ })).toBeNull();
  expect((operations().getByRole("button", { name: /修复/ }) as HTMLButtonElement).disabled).toBe(
    false,
  );
  expect(
    (screen.getByRole("checkbox", { name: /我已确认操作范围/ }) as HTMLInputElement).checked,
  ).toBe(false);
  execute("确认执行 · 修复");
  await waitFor(() => expect(api.run).toHaveBeenCalledWith("repair", false));
});
test("newer package offers upgrade and uninstall; uninstall returns to installation", async () => {
  const api = bridge({ ...installed, candidate_version: "0.2.0", deployment_action: "upgrade" });
  vi.mocked(api.run).mockResolvedValue(empty);
  render(<Installer bridge={api} />);
  await screen.findByText("/applications/terminal");
  expect(screen.getByRole("heading", { level: 1 }).textContent).toBe("升级 Terminal");
  expect((operations().getByRole("button", { name: /升级/ }) as HTMLButtonElement).disabled).toBe(
    false,
  );
  expect(operations().queryByRole("button", { name: /修复|安装/ })).toBeNull();
  fireEvent.click(operations().getByRole("button", { name: /卸载/ }));
  expect(
    (screen.getByRole("checkbox", { name: "保留配置与业务数据" }) as HTMLInputElement).checked,
  ).toBe(true);
  execute("确认执行 · 卸载");
  await screen.findByText("操作已完成。用户资料已保留。");
  expect(api.run).toHaveBeenCalledWith("uninstall", false);
  expect(screen.getByRole("heading", { level: 1 }).textContent).toBe("安装 Terminal");
  expect(operations().getAllByRole("button")).toHaveLength(1);
});

test("deleting data is opt-in and changing retention clears the previous confirmation", async () => {
  const api = bridge(installed);
  vi.mocked(api.run).mockResolvedValue(empty);
  render(<Installer bridge={api} />);
  await screen.findByText("/applications/terminal");
  expect(screen.queryByRole("checkbox", { name: "保留配置与业务数据" })).toBeNull();
  fireEvent.click(operations().getByRole("button", { name: /卸载/ }));
  fireEvent.click(screen.getByRole("checkbox", { name: /我已确认操作范围/ }));
  fireEvent.click(screen.getByRole("checkbox", { name: "保留配置与业务数据" }));
  expect(screen.getByRole("alert").textContent).toContain("/data/node");
  expect(
    (screen.getByRole("checkbox", { name: /我已确认永久删除/ }) as HTMLInputElement).checked,
  ).toBe(false);
  expect(
    (screen.getByRole("button", { name: "确认执行 · 卸载" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  fireEvent.click(screen.getByRole("checkbox", { name: /我已确认永久删除/ }));
  fireEvent.click(screen.getByRole("button", { name: "确认执行 · 卸载" }));
  await screen.findByText("卸载已完成，所选本机配置与业务数据已删除。");
  expect(api.run).toHaveBeenCalledWith("uninstall", true);
});

test("resuming uninstall retains its recorded deletion choice", async () => {
  const api = bridge({
    ...installed,
    pending: "uninstall",
    deployment_action: "",
    preserve_data: false,
  });
  vi.mocked(api.run).mockResolvedValue(empty);
  render(<Installer bridge={api} />);
  await screen.findByText("/applications/terminal");
  const preserve = screen.getByRole("checkbox", { name: "保留配置与业务数据" }) as HTMLInputElement;
  expect(preserve.checked).toBe(false);
  expect(preserve.disabled).toBe(true);
  fireEvent.click(operations().getByRole("button", { name: /卸载/ }));
  expect(preserve.checked).toBe(false);
  fireEvent.click(screen.getByRole("checkbox", { name: /我已确认永久删除/ }));
  fireEvent.click(screen.getByRole("button", { name: "继续执行 · 卸载" }));
  await waitFor(() => expect(api.run).toHaveBeenCalledWith("uninstall", true));
});
test.each([true, false])(
  "uninstall remains available when package cannot repair (source %s)",
  async source => {
    const api = bridge({
      ...installed,
      source_available: source,
      candidate_version: source ? "0.0.9" : "",
      deployment_action: "",
    });
    render(<Installer bridge={api} />);
    await screen.findByText("/applications/terminal");
    expect((operations().getByRole("button", { name: /修复/ }) as HTMLButtonElement).disabled).toBe(
      true,
    );
    expect((operations().getByRole("button", { name: /卸载/ }) as HTMLButtonElement).disabled).toBe(
      false,
    );
    expect(
      (screen.getByRole("button", { name: "打开 Terminal" }) as HTMLButtonElement).disabled,
    ).toBe(false);
    if (source) expect(screen.getByText(/已安装版本比此安装包更新/)).toBeTruthy();
    else expect(screen.getByText(/未提供完整安装源/)).toBeTruthy();
    expect(api.run).not.toHaveBeenCalled();
  },
);
test("active Terminal blocks maintenance", async () => {
  const api = bridge({ ...installed, busy: ["terminal"] });
  render(<Installer bridge={api} />);
  await screen.findByText("/applications/terminal");
  for (const button of operations().getAllByRole("button"))
    expect((button as HTMLButtonElement).disabled).toBe(true);
  expect(
    (screen.getByRole("button", { name: "确认执行 · 修复" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  expect(api.run).not.toHaveBeenCalled();
});
test("unfinished maintenance only offers explicit resume and displays executor failure", async () => {
  const api = bridge({ ...installed, pending: "repair" });
  vi.mocked(api.run).mockRejectedValue(new Error("launcher registration failed: /node/logs"));
  render(<Installer bridge={api} />);
  await screen.findByText("/applications/terminal");
  expect(screen.getByRole("heading", { level: 1 }).textContent).toBe("继续维护");
  expect(operations().getAllByRole("button")).toHaveLength(1);
  expect(operations().queryByRole("button", { name: /卸载/ })).toBeNull();
  execute("继续执行 · 修复");
  await screen.findByText("launcher registration failed: /node/logs");
  fireEvent.click(screen.getByRole("button", { name: "打开日志目录" }));
  expect(api.run).toHaveBeenCalledWith("repair", false);
  expect(api.open).toHaveBeenCalledWith("logs");
});

test("recorded pending upgrade remains explicitly resumable", async () => {
  const api = bridge({ ...installed, pending: "upgrade", deployment_action: "" });
  render(<Installer bridge={api} />);
  await screen.findByText("/applications/terminal");
  expect(api.run).not.toHaveBeenCalled();
  execute("继续执行 · 升级");
  await waitFor(() => expect(api.run).toHaveBeenCalledWith("upgrade", false));
});
