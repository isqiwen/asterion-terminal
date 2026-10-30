// The startup screen opens the workbench by itself once every step has
// passed, unless it has something to point out; then it waits for the user.
async function enterWorkbench(page, timeout = 60000) {
  const enter = page.getByRole("button", { name: /^(进入工作台|ENTER WORKBENCH)$/ });
  const workbench = page.locator(".workspace-tabs");
  try {
    await enter.or(workbench).first().waitFor({ timeout });
  } catch (error) {
    // The startup screen names the failing step and its cause.
    console.error("Startup screen:", await page.locator("body").innerText());
    throw error;
  }
  if (await enter.isVisible()) await enter.click();
  await workbench.waitFor({ timeout });
}
// What marks a root as a test fixture: its capacity is stated, so what the
// services admit does not depend on the machine running the test.
const testNode = { ASTERION_TEST_NODE_ISOLATED: "1" };
module.exports = { enterWorkbench, testNode };
