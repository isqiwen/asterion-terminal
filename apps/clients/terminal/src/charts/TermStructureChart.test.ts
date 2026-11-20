import { expect, test } from "vitest";
import { deliveryMonthIndex } from "./TermStructureChart";

test("delivery months place four-digit and three-digit symbols on one calendar axis", () => {
  const oct26 = deliveryMonthIndex("rb2610", 2026);
  const jan27 = deliveryMonthIndex("rb2701", 2026);
  const czceJan27 = deliveryMonthIndex("OI701", 2026);
  expect(oct26).not.toBeNull();
  expect(jan27! - oct26!).toBe(3);
  expect(czceJan27).toBe(jan27);
});
