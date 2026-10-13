import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { NodeIdentity } from "./NodeIdentity";

const profile = {
  id: "research-1",
  host: "192.0.2.10",
  ssh_port: "22",
  username: "asterion",
  known_hosts: "/home/owner/known_hosts",
  agent_port: "7442",
};
const button = () =>
  screen.getByRole("button", { name: "停止服务并更换身份" }) as HTMLButtonElement;

test("an identity is replaced only after the owner confirms that the machine's services stop", async () => {
  const run = vi.fn(async () => true);
  const replaced = vi.fn();
  render(
    <NodeIdentity
      profile={profile}
      busy={false}
      run={run}
      onCancel={() => {}}
      onReplaced={replaced}
    />,
  );
  expect(button().disabled).toBe(true);
  fireEvent.click(screen.getByRole("checkbox", { name: "我已了解这台机器上的服务会停止" }));
  expect(button().disabled).toBe(false);
  // A pasted key is required before it can be used, and is sent once.
  fireEvent.change(screen.getByLabelText("SSH 密钥来源"), { target: { value: "provided" } });
  expect(button().disabled).toBe(true);
  fireEvent.change(screen.getByLabelText("SSH 私钥"), { target: { value: "PRIVATE KEY" } });
  fireEvent.click(button());
  // The Agent's port is the enrollment's, not the form's.
  expect(run).toHaveBeenCalledWith("node.identity.replace", {
    id: "research-1",
    host: "192.0.2.10",
    ssh_port: "22",
    username: "asterion",
    known_hosts: "/home/owner/known_hosts",
    key_source: "provided",
    private_key: "PRIVATE KEY",
  });
  expect((screen.getByLabelText("SSH 私钥") as HTMLTextAreaElement).value).toBe("");
  await vi.waitFor(() => expect(replaced).toHaveBeenCalledTimes(1));
});
