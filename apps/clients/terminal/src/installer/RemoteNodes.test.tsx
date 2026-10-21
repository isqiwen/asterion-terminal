import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { test, expect, vi } from "vitest";
import "./Installer";
import { RemoteNodes } from "./RemoteNodes";
const node = {
  id: "research",
  host: "node.example",
  port: 46000,
  directory: "/var/lib/asterion/nodes/research",
  installed: true,
};
test("remote removal requires node selection and renewed confirmation when deletion is selected", async () => {
  const request = vi.fn().mockResolvedValue({ nodes: [node], pending: null });
  render(<RemoteNodes request={request} busy={false} setBusy={() => {}} />);
  fireEvent.click(screen.getByRole("button", { name: "读取节点与维护进度" }));
  await screen.findByText(/research · node.example/);
  const remove = screen.getByRole("button", { name: "卸载所选远程节点" }) as HTMLButtonElement;
  expect(remove.disabled).toBe(true);
  fireEvent.click(screen.getByRole("checkbox", { name: /research · node.example/ }));
  fireEvent.click(screen.getByRole("checkbox", { name: /我已确认远程节点/ }));
  expect(remove.disabled).toBe(false);
  fireEvent.click(screen.getByRole("checkbox", { name: "保留所选远程节点的配置与业务数据" }));
  expect(remove.disabled).toBe(true);
  expect(screen.getByRole("alert").textContent).toContain("永久删除");
  fireEvent.click(screen.getByRole("checkbox", { name: /我已确认远程节点/ }));
  fireEvent.click(remove);
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith({
      maintenance: { action: "UNINSTALL", nodes: ["research"], removeData: true },
    }),
  );
});
test("pending deletion retains its original scope and displays the failing node", async () => {
  const request = vi.fn().mockResolvedValue({
    nodes: [node],
    pending: {
      action: "uninstall",
      version: "0.1.0",
      selection: ["research"],
      remove_data: true,
      nodes: [{ id: "research", phase: "removing", error: "host unreachable" }],
    },
  });
  render(<RemoteNodes request={request} busy={false} setBusy={() => {}} />);
  fireEvent.click(screen.getByRole("button", { name: "读取节点与维护进度" }));
  await screen.findByText("host unreachable");
  const retain = screen.getByRole("checkbox", {
    name: "保留所选远程节点的配置与业务数据",
  }) as HTMLInputElement;
  expect(retain.checked).toBe(false);
  expect(retain.disabled).toBe(true);
  expect(screen.queryByRole("button", { name: "完成远程启动与确认" })).toBeNull();
});

test("maintenance edits an existing connection without provisioning nodes", async () => {
  const request = vi.fn().mockResolvedValue({ nodes: [node], pending: null });
  render(<RemoteNodes request={request} busy={false} setBusy={() => {}} />);
  const save = screen.getByRole("button", {
    name: "保存该节点 SSH 连接信息",
    hidden: true,
  }) as HTMLButtonElement;
  expect(save.disabled).toBe(true);
  fireEvent.click(screen.getByRole("button", { name: "读取节点与维护进度" }));
  await screen.findByText(/research · node.example/);
  fireEvent.change(screen.getByRole("combobox"), { target: { value: "research" } });
  fireEvent.click(screen.getByText("SSH 维护连接"));
  fireEvent.change(screen.getByLabelText("本机 known_hosts 文件绝对路径"), {
    target: { value: "/home/operator/.ssh/known_hosts" },
  });
  fireEvent.click(save);
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith({
      maintenance: {
        action: "CONFIGURE",
        connections: [
          expect.objectContaining({
            id: "research",
            host: "node.example",
            knownHosts: "/home/operator/.ssh/known_hosts",
          }),
        ],
      },
    }),
  );
  expect(screen.queryByRole("button", { name: "安装远程 Agent" })).toBeNull();
});
