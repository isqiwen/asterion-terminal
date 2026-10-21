import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test } from "vitest";
import { readySnapshot } from "../testing/core";
import { NodeResources } from "./NodeResources";

test("shows measured resources and labels retained observations when the node goes offline", () => {
  const node = readySnapshot().nodes[0];
  node.health!.host_resources = {
    sampled_at_ms: Date.now(),
    cpu_usage_percent: 25,
    memory_available_bytes: 4 * 1024 ** 3,
    disk_available_bytes: 80 * 1024 ** 3,
    disk_path: "/node/data",
    failure: null,
  };
  const { rerender } = render(<NodeResources node={node} />);
  expect(screen.getByText(/主机 CPU：25.0%/).textContent).toContain("4.0 GiB");
  expect(screen.getByText(/主机 CPU：25.0%/).textContent).toContain("80.0 GiB");
  expect(screen.getByText("磁盘观测目录：/node/data")).toBeTruthy();
  node.state = "unreachable";
  rerender(<NodeResources node={node} />);
  expect(screen.getByText(/节点离线，主机资源上次观测/)).toBeTruthy();
});

test("missing, initial and failed samples are not shown as zero usage", () => {
  const node = readySnapshot().nodes[0];
  const { rerender } = render(<NodeResources node={node} />);
  expect(screen.getByText("主机资源尚无观测")).toBeTruthy();
  node.health!.host_resources = {
    sampled_at_ms: Date.now(),
    cpu_usage_percent: null,
    memory_available_bytes: 1024 ** 3,
    disk_available_bytes: 0,
    disk_path: "/node/data",
    failure: null,
  };
  rerender(<NodeResources node={node} />);
  expect(screen.getByText(/等待 CPU 采样区间/)).toBeTruthy();
  node.health!.host_resources.failure = {
    code: "unavailable",
    message: "cannot sample node resources: permission denied",
  };
  rerender(<NodeResources node={node} />);
  expect(screen.getByText("节点资源观测不可用")).toBeTruthy();
  expect(screen.queryByText(/主机 CPU：/)).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "详情" }));
  expect(screen.getByText(/permission denied/)).toBeTruthy();
});
