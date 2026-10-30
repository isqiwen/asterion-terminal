import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { beforeEach, expect, test, vi } from "vitest";
import { request, type BackupReport } from "../bridge/client";
import { readySnapshot } from "../testing/core";
import { DataBackup } from "./DataBackup";
vi.mock("../bridge/client", async importOriginal => ({
  ...(await importOriginal<typeof import("../bridge/client")>()),
  request: vi.fn(),
}));
const report: BackupReport = {
  manifest: {
    backup_id: "reviewed-operation",
    owners: [{ node: "fixture", service: "data-one", domain: "data" }],
    files: [],
    dependencies: [{ included: false }],
    limitations: [
      { owner: 0, reason: "original_execution_program_identity_not_recorded" },
      { owner: 0, reason: "original_acquisition_program_identity_not_recorded" },
    ],
    exclusions: [],
  },
  partial: true,
  preview_digest: "a".repeat(64),
};
function snapshot(running = false) {
  const value = readySnapshot();
  const node = value.nodes[0];
  node.health!.services = [
    {
      ...node.health!.services[0],
      id: "data-one",
      kind: "data",
      desired_running: running,
      pid: running ? 123 : 0,
    },
  ];
  return value;
}
beforeEach(() => vi.mocked(request).mockReset());
test("running owners cannot be previewed or stopped by the backup screen", () => {
  render(<DataBackup snapshot={snapshot(true)} />);
  fireEvent.click(screen.getByRole("checkbox", { name: /data-one/ }));
  expect((screen.getByRole("button", { name: "预览备份" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
  expect(request).not.toHaveBeenCalled();
});
test("partial scope requires explicit review and saves the exact reviewed identity", async () => {
  vi.mocked(request).mockResolvedValue({ ...snapshot(), backup: report });
  render(<DataBackup snapshot={snapshot()} />);
  fireEvent.click(screen.getByRole("checkbox", { name: /data-one/ }));
  fireEvent.change(screen.getByLabelText("新备份目录"), {
    target: { value: "/isolated/new-backup" },
  });
  fireEvent.click(screen.getByRole("button", { name: "预览备份" }));
  await screen.findByText("部分备份：请核对缺失依赖与限制");
  expect(screen.getByText(/未记录原 Research 执行程序身份/)).toBeTruthy();
  expect(screen.getByText(/未记录原 Data 采集主程序身份/)).toBeTruthy();
  expect((screen.getByRole("button", { name: "保存备份" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
  fireEvent.click(screen.getByRole("checkbox", { name: "已核对所选范围、缺失依赖和新目录" }));
  vi.mocked(request).mockResolvedValue({ ...snapshot(), backup: { ...report, saved: true } });
  fireEvent.click(screen.getByRole("button", { name: "保存备份" }));
  await screen.findByText("备份已保存并核验");
  expect(request).toHaveBeenLastCalledWith("system.backup.save", {
    operation_id: "reviewed-operation",
    services: ["data-one"],
    destination: "/isolated/new-backup",
    preview_digest: report.preview_digest,
  });
});
test("changing inspected source invalidates restore authorization", async () => {
  vi.mocked(request).mockResolvedValue({ ...snapshot(), backup: report });
  render(<DataBackup snapshot={snapshot()} />);
  fireEvent.change(screen.getByLabelText("已有备份目录"), {
    target: { value: "/isolated/backup" },
  });
  fireEvent.click(screen.getByRole("button", { name: "核验备份" }));
  await screen.findByLabelText("新恢复目录");
  fireEvent.change(screen.getByLabelText("新恢复目录"), { target: { value: "/isolated/restore" } });
  fireEvent.click(
    screen.getByRole("checkbox", { name: "确认恢复到独立新目录，不覆盖原数据或恢复授权" }),
  );
  expect((screen.getByRole("button", { name: "恢复到新目录" }) as HTMLButtonElement).disabled).toBe(
    false,
  );
  fireEvent.change(screen.getByLabelText("已有备份目录"), { target: { value: "/isolated/other" } });
  await waitFor(() => expect(screen.queryByRole("button", { name: "恢复到新目录" })).toBeNull());
  expect(request).toHaveBeenCalledTimes(1);
});
