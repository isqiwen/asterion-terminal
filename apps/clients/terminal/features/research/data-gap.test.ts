import { expect, test } from "vitest";
import { Period } from "../../src/generated/asterion/common_pb";
import { BackendError } from "../../src/i18n/errors";
import { dataGapForSymbol, dataGapParams, isMinutePeriod, parseDataGap } from "./data-gap";

test("parseDataGap maps acquirable minute and daily diagnostics to dataset coverage targets", () => {
  expect(
    parseDataGap(
      new BackendError(
        "operation_failed",
        "minutes of SHFE/rb/2026-05 are not stored on 2026-01-05",
      ),
      { minutes: true },
    ),
  ).toEqual({
    kind: "minutes",
    exchange: "SHFE",
    product: "SHFE/rb",
    search: "rb2605",
  });

  expect(
    parseDataGap(
      "the outgoing month has no bars from the roll day on: SHFE/rb/2026-05 from 2026-03-16",
      {
        minutes: true,
      },
    ),
  ).toEqual({
    kind: "minutes",
    exchange: "SHFE",
    product: "SHFE/rb",
    search: "rb2605",
  });

  expect(parseDataGap("daily bars of DCE are not stored on 2026-01-05")).toEqual({
    kind: "daily",
    exchange: "DCE",
    product: "",
    search: "",
  });

  expect(parseDataGap("no settlement parameters of SHFE/cu/2026-04 on 2026-01-05")).toEqual({
    kind: "daily",
    exchange: "SHFE",
    product: "SHFE/cu",
    search: "cu2604",
  });

  expect(dataGapParams(dataGapForSymbol("minutes", "SHFE/rb"))).toEqual({
    kind: "minutes",
    exchange: "SHFE",
    product: "SHFE/rb",
    search: "",
  });
});

test("parseDataGap excludes non-acquirable daily zero-volume and logic errors", () => {
  expect(
    parseDataGap(
      new BackendError(
        "operation_failed",
        "the outgoing month has no bars from the roll day on: SHFE/wr/2023-10 from 2023-10-16",
      ),
      { minutes: false },
    ),
  ).toBeNull();
  expect(parseDataGap("a dominant series requires at least two month contracts")).toBeNull();
  expect(parseDataGap("the factor failed on SHFE/rb: ZeroDivisionError")).toBeNull();
  expect(isMinutePeriod(Period.MINUTES_5)).toBe(true);
  expect(isMinutePeriod(Period.DAY)).toBe(false);
  expect(isMinutePeriod(Period.UNSPECIFIED)).toBe(false);
});
