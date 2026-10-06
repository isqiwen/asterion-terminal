// The startup screen opens the workbench by itself once every step has
// passed, unless it has something to point out; then it waits for the user.
async function enterWorkbench(page, timeout = 60000) {
  const enter = page.getByRole("button", { name: /^(进入工作台|ENTER WORKBENCH)$/ });
  const workbench = page.locator(".workspace-tabs");
  await enter.or(workbench).first().waitFor({ timeout });
  if (await enter.isVisible()) await enter.click();
  await workbench.waitFor({ timeout });
}
// What marks a node as a test fixture. It states its capacity, so what its
// Agent admits does not depend on the machine running the test.
const testNode = { ASTERION_TEST_NODE_ISOLATED: "1", ASTERION_TEST_HOST_CAPACITY: "10,16384" };
module.exports = { enterWorkbench, testNode };
