import type { Page, Locator } from "@playwright/test";
export async function openSettingsWindow(page: Page, trigger?: Locator) {
  const opened = page.waitForEvent("popup");
  await (trigger ?? page.getByRole("button", {name: /^(设置|Settings)$/, exact:true})).click();
  const settings = await opened;
  await settings.waitForLoadState();
  return settings;
}
export async function closeSettingsWindow(page: Page) {
  const main = await page.opener();
  if (!main) throw new Error("Settings must have a workbench opener");
  const closed = page.waitForEvent("close");
  await page.getByRole("button", {name:/^(关闭设置|Close settings)$/}).click();
  await closed;
  return main;
}
