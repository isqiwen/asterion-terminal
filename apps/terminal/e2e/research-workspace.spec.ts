import { strategies } from "./strategy-fixture";
import { rulesRoute, selectRules } from "./rules-fixture";
import { test, expect } from "@playwright/test";
import { nativeContext } from "./native";

test("draft restart, reusable templates, missing fixed version and conflict recovery", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  let draft: any = null;
  const templates = new Map<string, any>();
  let conflict = false,
    missing = false;
  const version = {
    id: "fixed-history",
    dataset_id: "daily-dataset",
    rows: 40,
    manifest: {
      source: "tushare",
      first: "2024-01-01",
      last: "2024-03-01",
      scope: { contract: "SHFE.rb2405" },
      version_semantics: "CUMULATIVE",
    },
  };
  await context.route("**/api/v1/**", (route) => {
    if (new URL(route.request().url()).pathname.endsWith("/research/experiments")) return route.fulfill({ json: { items: [] } });
    if (
      new URL(route.request().url()).pathname.endsWith("/research/strategies")
    )
      return route.fulfill({ json: strategies });
    if (route.request().url().endsWith("/contract-rules"))
      return rulesRoute(route);
    if (route.request().url().endsWith("/access/scopes"))
      return route.fulfill({
        json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 },
      });

    const req = route.request(),
      url = new URL(req.url()),
      path = url.pathname;
    if (path.includes("/security")) return route.fallback();
    if (
      path.includes("/research/workspace") ||
      path.includes("/research/templates")
    )
      expect(url.searchParams.get("expected_account")).toBe(
        "draft@example.com",
      );
    if (path.endsWith("/research/workspace"))
      return route.fulfill({
        json: { draft, templates: [...templates.values()] },
      });
    if (path.endsWith("/research/workspace/draft")) {
      const body = req.postDataJSON();
      if (conflict || body.expected_revision !== (draft?.revision || 0))
        return route.fulfill({
          status: 409,
          json: { detail: "另一窗口已修改此草稿" },
        });
      draft = {
        id: "draft",
        name: "研究草稿",
        revision: (draft?.revision || 0) + 1,
        content: body.content,
        updated_at: 1,
      };
      return route.fulfill({ json: draft });
    }
    if (path.includes("/research/templates/")) {
      const id = path.split("/templates/")[1].split("/")[0];
      if (path.endsWith("/delete")) {
        templates.delete(id);
        return route.fulfill({ json: { status: "deleted" } });
      }
      const body = req.postDataJSON(),
        value = {
          id,
          ...body,
          revision: (templates.get(id)?.revision || 0) + 1,
          updated_at: 1,
        };
      templates.set(id, value);
      return route.fulfill({ json: value });
    }
    if (path.includes("/data/catalog"))
      return route.fulfill({ json: { items: [version], total: 1, offset: 0 } });
    if (path.includes("/data/versions/"))
      return missing
        ? route.fulfill({ status: 404, json: { detail: "原版本文件缺失" } })
        : route.fulfill({ json: { version, rows: [], total: 40 } });
    const user = {
      email: "draft@example.com",
      first_name: "Draft",
      last_name: "Test",
    };
    return route.fulfill({
      json: path.endsWith("/login")
        ? { session: "session", user }
        : path.endsWith("/me")
          ? user
          : path.endsWith("/health")
            ? { status: "ready" }
            : [],
    });
  });
  await page.goto("/");
  await page.getByLabel("邮箱", { exact: true }).fill("draft@example.com");
  await page.getByLabel("密码", { exact: true }).fill("test-draft-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page.getByRole("button", { name: "研究", exact: true }).click();
  await page
    .getByRole("combobox", { name: "策略", exact: true })
    .selectOption({ label: "双均线 · 1.0.0" });
  await page
    .getByRole("combobox", { name: "日线数据集", exact: true })
    .selectOption(version.dataset_id);
  await page.getByLabel("快均线周期", { exact: true }).fill("7");
  await selectRules(page);
  await expect.poll(() => draft?.content.config.parameters.fast).toBe(7);
  await page.getByText("实验模板管理", { exact: true }).click();
  await page.getByLabel("模板名称", { exact: true }).fill("日线研究模板");
  await page.getByRole("button", { name: "保存为新模板", exact: true }).click();
  await expect.poll(() => templates.size).toBe(1);
  const templateId = [...templates.keys()][0];
  await page.reload();
  await page.getByRole("button", { name: "研究", exact: true }).click();
  await expect(page.getByLabel("快均线周期", { exact: true })).toHaveValue("7");
  await expect(page.getByLabel("固定合约规则")).toContainText("乘数 10");
  await expect(
    page.getByRole("checkbox", { name: /我接受历史收盘/ }),
  ).not.toBeChecked();
  await expect(
    page.getByRole("combobox", { name: "固定数据版本", exact: true }),
  ).toHaveValue(version.id);
  await page.getByLabel("快均线周期", { exact: true }).fill("9");
  await page.getByText("实验模板管理", { exact: true }).click();
  await page
    .getByRole("combobox", { name: "实验模板", exact: true })
    .selectOption(templateId);
  await page.getByRole("button", { name: "载入模板", exact: true }).click();
  await expect(page.getByLabel("快均线周期", { exact: true })).toHaveValue("7");
  await page.getByLabel("模板名称", { exact: true }).fill("重命名模板");
  await page.getByRole("button", { name: "更新所选模板", exact: true }).click();
  await expect.poll(() => templates.get(templateId)?.name).toBe("重命名模板");
  await page.getByRole("button", { name: "删除模板", exact: true }).click();
  await expect.poll(() => templates.size).toBe(0);
  conflict = true;
  await page.getByLabel("快均线周期", { exact: true }).fill("11");
  await expect(page.getByRole("alert")).toContainText("另一窗口");
  await expect(page.getByLabel("快均线周期", { exact: true })).toHaveValue(
    "11",
  );
  expect(draft.content.config.parameters.fast).not.toBe(11);
  conflict = false;
  await page
    .getByRole("button", { name: "载入服务器草稿", exact: true })
    .click();
  await expect(page.getByLabel("快均线周期", { exact: true })).toHaveValue("7");
  missing = true;
  await page.reload();
  await page.getByRole("button", { name: "研究", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("原版本文件缺失");
  await expect(page.getByLabel("固定合约规则")).toContainText("乘数 10");
  await expect(
    page.getByRole("button", { name: "运行回测", exact: true }),
  ).toBeDisabled();
  missing = false;
  draft.content.config.strategy = {
    ...strategies[0].identity,
    digest: "c".repeat(64),
  };
  await page.reload();
  await page.getByRole("button", { name: "研究", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText(
    "原策略未安装或实现已变化",
  );
  await expect(
    page.getByRole("button", { name: "运行回测", exact: true }),
  ).toBeDisabled();
  await page
    .getByRole("combobox", { name: "策略", exact: true })
    .selectOption({ label: "双均线 · 1.0.0" });
  await expect(page.getByLabel("快均线周期", { exact: true })).toHaveValue("5");
  await expect
    .poll(() => draft.content.config.strategy.digest)
    .toBe(strategies[0].identity.digest);
});
