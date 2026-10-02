import { test, expect, type Page } from "@playwright/test";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";

async function fits(page: Page, selector: string) {
  const area = page.locator(selector);
  await expect(area).toBeVisible();
  const overflow = await area.evaluate(el =>
    [...el.querySelectorAll("*")]
      .filter(child => {
        const r = child.getBoundingClientRect();
        return r.width && r.right > el.getBoundingClientRect().right + 1;
      })
      .map(child => ({
        tag: child.tagName,
        class: child.className,
        text: child.textContent?.slice(0, 80),
      })),
  );
  expect(
    await area.evaluate(el => el.scrollWidth <= el.clientWidth + 1),
    JSON.stringify(overflow),
  ).toBe(true);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
}

for (const locale of ["zh-CN", "en-US"]) {
  test(`workspaces and settings fit desktop sizes in ${locale}`, async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", e => errors.push(e.message));
    await page.addInitScript(locale => localStorage.setItem("asterion.locale", locale), locale);
    await page.goto("/");
    await expect(page.locator(".activity-rail")).toBeVisible();
    const entries = page.locator(".workspace-tabs").getByRole("button");
    // Use the host's semantic nav container; titles remain plugin-owned and localized.
    for (const width of [1440, 800]) {
      await page.setViewportSize({ width, height: 900 });
      for (let i = 0; i < (await entries.count()); i++) {
        await entries.nth(i).click();
        await expect(
          page.locator(".terminal-business > :not([role=status])").first(),
        ).toBeVisible();
        await fits(page, ".terminal-business");
        await page.screenshot({
          path: `apps/clients/terminal/test-results/ui-${locale}-${width}-${i}.png`,
        });
      }
    }
    const settings = await openSettingsWindow(page);
    await settings.setViewportSize({ width: 640, height: 440 });
    for (let i = 0; i < 4; i++) {
      await settings.locator(".settings-sidebar nav button").nth(i).click();
      await fits(settings, ".settings-content");
      await settings.screenshot({
        path: `apps/clients/terminal/test-results/ui-${locale}-settings-${i}.png`,
      });
    }
    page = await closeSettingsWindow(settings);
    await page.emulateMedia({ reducedMotion: "reduce" });
    await expect(page.locator(".activity-rail button").first()).toHaveCSS(
      "transition-duration",
      "0s",
    );
    expect(errors).toEqual([]);
  });
}
