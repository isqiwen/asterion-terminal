import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";
import type { SetupProgress } from "../src/startup/Setup";

test("first setup waits for consent, shows download progress, retries and enters only when ready", async ({
  page,
  context,
}) => {
  let status: SetupProgress = {
    running: false,
    ready: false,
    step: 0,
    downloaded: 0,
    network: null,
    dependencies: { total: null, installed: 0, phase: "", current: "" },
    total: null,
    error: "",
    directory: "/home/test/.local/share/me.asterion.terminal/runtime",
  };
  let calls = 0;
  let complete: (() => void) | undefined;
  await nativeContext(context, {
    desktop_setup_status: () => ({
      ...status,
      network: status.network
        ? { ...status.network, sampled_ms: Date.now() }
        : null,
    }),
    desktop_setup_install: async () => {
      calls++;
      status = {
        ...status,
        running: true,
        error: "",
        step: 1,
        network: { received: 0, sent: 0, interfaces: "test", sampled_ms: 0 },
        downloaded: 25,
        total: 100,
      };
      await new Promise<void>((resolve) => {
        complete = resolve;
      });
      status =
        calls === 1
          ? { ...status, running: false, error: "下载校验失败，请重试" }
          : { ...status, ready: true, running: false, step: 5 };
      return status;
    },
    desktop_session: () => {
      throw new Error("fixture: services requested after setup");
    },
  });
  await page.setViewportSize({ width: 800, height: 640 });
  await page.goto("/");
  await expect(page.getByRole("button", { name: "开始设置" })).toBeEnabled();
  expect(calls).toBe(0);
  await expect(page.locator("footer")).toContainText(
    "~/.local/share/me.asterion.terminal",
  );
  await expect(page.getByLabel("安装状态")).toHaveCount(0);
  await page.getByRole("combobox", { name: "语言" }).selectOption("en");
  await expect(page.getByRole("button", { name: "BEGIN SETUP" })).toBeVisible();
  await page.screenshot({ path: "../../.state/first-setup-en.png" });
  await page.getByRole("combobox", { name: "Language" }).selectOption("zh");
  await page.screenshot({ path: "../../.state/first-setup-zh.png" });
  await page.getByRole("button", { name: "开始设置" }).click();
  await expect(page.getByRole("button", { name: "正在设置" })).toBeDisabled();
  await expect(
    page.getByRole("progressbar", { name: "安装 Python 运行时" }),
  ).toHaveAttribute("aria-valuenow", "25");
  expect(calls).toBe(1);
  await expect(page.getByTestId("setup-elapsed")).not.toHaveText("耗时: 0s");
  for (const bytes of [1048576, 3145728, 3670016]) {
    status = {
      ...status,
      network: {
        received: bytes,
        sent: bytes / 20,
        interfaces: "test",
        sampled_ms: bytes / 1000,
      },
    };
    await expect
      .poll(async () =>
        Number(await page.getByTestId("setup-speed").getAttribute("data-rate")),
      )
      .toBeGreaterThan(0);
    await page.waitForTimeout(1100);
  }
  await expect(page.locator(".setup-speed-line")).toHaveAttribute("d", /L/);
  const stats = await page.getByLabel("安装状态").boundingBox();
  expect(stats!.y + stats!.height).toBeLessThanOrEqual(640);
  status = {
    ...status,
    network: {
      received: 10000000,
      sent: 500000,
      interfaces: "test",
      sampled_ms: 10000,
    },
  };
  await expect
    .poll(async () =>
      Number(await page.getByTestId("setup-speed").getAttribute("data-rate")),
    )
    .toBeGreaterThan(0);
  await expect(page.getByTestId("setup-upload")).toContainText(
    /↑ [0-9.]+ (KB|MB|B)\/s/,
  );
  await page.screenshot({ path: "../../.state/first-setup-running-zh.png" });
  await page.getByRole("combobox", { name: "语言" }).selectOption("en");
  await expect(page.getByTestId("setup-elapsed")).toContainText("Elapsed");
  await expect(
    page.getByRole("button", { name: "SETTING UP...", exact: true }),
  ).toBeDisabled();
  await expect(page.locator(".setup-status-done").first()).toHaveText("DONE");
  await expect(page.locator(".setup-status-done").first()).toHaveCSS(
    "color",
    "rgb(63, 186, 128)",
  );
  await page.screenshot({ path: "../../.state/first-setup-running-en.png" });
  await page.getByRole("combobox", { name: "Language" }).selectOption("zh");
  await expect(page.getByTestId("setup-speed")).toHaveAttribute(
    "data-rate",
    "0",
  );
  status = {
    ...status,
    network: null,
    dependencies: { total: null, installed: 0, phase: "", current: "" },
    total: null,
  };
  await expect(page.getByTestId("setup-speed")).toHaveText("↓ —");
  await expect(page.getByTestId("setup-upload")).toHaveText("↑ —");
  status = {
    ...status,
    step: 3,
    dependencies: {
      total: 27,
      installed: 0,
      phase: "downloading",
      current: "pyarrow, pydantic-core",
    },
  };
  await expect(page.getByTestId("setup-detail")).toContainText(
    "正在下载: pyarrow, pydantic-core",
  );
  await expect(page.getByTestId("setup-detail")).toContainText(
    "已安装依赖: 0/27",
  );
  await expect(
    page.getByRole("progressbar", { name: "安装运行依赖" }),
  ).toHaveAttribute("aria-valuenow", "0");
  await page.screenshot({
    path: "../../.state/first-setup-dependencies-zh.png",
  });
  await page.getByRole("combobox", { name: "语言" }).selectOption("en");
  await expect(page.getByTestId("setup-detail")).toContainText(
    "Dependencies installed: 0/27",
  );
  await page.screenshot({
    path: "../../.state/first-setup-dependencies-en.png",
  });
  await page.getByRole("combobox", { name: "Language" }).selectOption("zh");
  status = {
    ...status,
    dependencies: {
      ...status.dependencies,
      phase: "installed",
      installed: 27,
      current: "pyarrow==23.0.1",
    },
  };
  await expect(page.getByTestId("setup-detail")).toContainText(
    "已安装依赖: 27/27",
  );
  complete!();
  await expect(page.getByRole("alert")).toContainText("下载校验失败");
  await expect(page.getByLabel("安装状态")).toHaveCount(0);
  await page.getByRole("button", { name: "重试设置" }).click();
  await expect.poll(() => calls).toBe(2);
  await expect(page.getByTestId("setup-elapsed")).toHaveText("耗时: 0s");
  complete!();
  await expect(page.getByRole("button", { name: "进入工作台" })).toBeEnabled();
  await expect(page.getByRole("progressbar")).toHaveCount(5);
  for (const row of await page.getByRole("progressbar").all())
    await expect(row).toHaveAttribute("aria-valuenow", "100");
  await page.getByRole("button", { name: "进入工作台" }).click();
  await expect(page.getByRole("alert")).toContainText(
    "services requested after setup",
  );
});

test("completed runtime bypasses first setup without installing", async ({
  page,
  context,
}) => {
  let installs = 0;
  await nativeContext(context, {
    desktop_setup_install: () => {
      installs++;
      throw new Error("must not install");
    },
    desktop_session: () => {
      throw new Error("offline service fixture");
    },
  });
  await page.goto("/");
  await expect(page.getByRole("alert")).toContainText(
    "offline service fixture",
  );
  await expect(page.getByRole("main", { name: "首次设置" })).toHaveCount(0);
  expect(installs).toBe(0);
});
