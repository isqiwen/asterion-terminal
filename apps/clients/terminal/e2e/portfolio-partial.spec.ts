import { test } from "./test";
import { portfolioTargetsFlow } from "./portfolio-targets-flow";
test("daily partial portfolio orders expire honestly and new targets use original holdings through export and restore", async ({
  page,
}) => {
  test.setTimeout(120000);
  await portfolioTargetsFlow(page, true);
});
