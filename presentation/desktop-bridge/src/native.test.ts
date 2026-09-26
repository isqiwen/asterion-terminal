import { expect, test, vi } from "vitest";
const native = vi.hoisted(() => vi.fn(async () => ({ ok: true })));
vi.mock("@tauri-apps/api/core", () => ({ invoke: native }));
import { invoke } from "./native";
import { validateWire } from "@asterion/runtime-client/communication";

test("all native commands receive a fresh validated context", async () => {
  expect(
    await invoke("desktop_info", { communication: { version: 9 } }),
  ).toEqual({ ok: true });
  const [command, args] = native.mock.calls[0] as unknown as [
    string,
    { communication: unknown },
  ];
  expect(command).toBe("desktop_info");
  expect(() => validateWire("Context", args.communication)).not.toThrow();
});
