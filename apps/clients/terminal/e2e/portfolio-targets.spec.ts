import { test } from "./test";
import { portfolioTargetsFlow } from "./portfolio-targets-flow";
test("explicit portfolio policy preserves full original execution, export, restart and joint restore", async ({
  page,
}) => {
  test.setTimeout(120000);
  await portfolioTargetsFlow(page, false);
});
