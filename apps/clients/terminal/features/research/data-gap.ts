import { Period } from "../../src/generated/asterion/common_pb";
import { BackendError, type DisplayError } from "../../src/i18n/errors";

export type DataGapTarget = {
  kind: "daily" | "minutes";
  exchange: string;
  product: string;
  search: string;
};

export function isMinutePeriod(period: Period | undefined): boolean {
  return (
    period === Period.MINUTES_1 ||
    period === Period.MINUTES_5 ||
    period === Period.MINUTES_10 ||
    period === Period.MINUTES_15 ||
    period === Period.MINUTES_30 ||
    period === Period.MINUTES_60
  );
}

export function dataGapForSymbol(kind: "daily" | "minutes", symbol = ""): DataGapTarget {
  const trimmed = symbol.trim();
  const contract = /^([A-Z]+)\/([A-Za-z0-9]+)\/(\d{4})-(\d{2})$/.exec(trimmed);
  if (contract) {
    const [, exchange, code, year, month] = contract;
    const productCode = code!.toLowerCase();
    return {
      kind,
      exchange: exchange!,
      product: `${exchange}/${productCode}`,
      search: `${productCode}${year!.slice(2)}${month}`,
    };
  }
  const product = /^([A-Z]+)\/([A-Za-z0-9]+)$/.exec(trimmed);
  if (product) {
    const [, exchange, code] = product;
    return {
      kind,
      exchange: exchange!,
      product: `${exchange}/${code!.toLowerCase()}`,
      search: "",
    };
  }
  const exchange = /^([A-Z]{2,8})$/.exec(trimmed);
  if (exchange) {
    return { kind, exchange: exchange[1]!, product: "", search: "" };
  }
  return { kind, exchange: "", product: "", search: "" };
}

export function dataGapParams(target: DataGapTarget): Record<string, string> {
  return {
    kind: target.kind,
    exchange: target.exchange,
    product: target.product,
    search: target.search,
  };
}

const SYMBOL_TOKEN = "([A-Z]+(?:/[A-Za-z0-9]+(?:/\\d{4}-\\d{2})?)?)";
const MINUTE_PATTERNS = [
  new RegExp(`^minutes of ${SYMBOL_TOKEN} are not stored on `),
  new RegExp(`^no stored minutes of ${SYMBOL_TOKEN} fall inside`),
  new RegExp(`^no stored minutes bars for ${SYMBOL_TOKEN}$`),
];
const DAILY_PATTERNS = [
  new RegExp(`^daily bars of ${SYMBOL_TOKEN} are not stored on `),
  new RegExp(`^no stored daily bars for ${SYMBOL_TOKEN}$`),
  /^the calendar of ([A-Z]+) is (?:not stored|incomplete)/,
  new RegExp(`^no settlement parameters of ${SYMBOL_TOKEN} on `),
  new RegExp(`^the contract table has no (?:unique listed life of )?${SYMBOL_TOKEN}$`),
];
const OUTGOING_PATTERN = new RegExp(
  `^the outgoing month has no bars from the roll day on: ${SYMBOL_TOKEN} from `,
);

// Matches only Lab diagnostics caused by unacquired data, never illiquid daily zero-volume bars.
export function parseDataGap(
  error: DisplayError,
  options?: { minutes?: boolean; fallbackSymbol?: string },
): DataGapTarget | null {
  const message =
    error instanceof BackendError
      ? error.diagnostic
      : error instanceof Error
        ? error.message
        : error;
  if (!message) return null;
  for (const pattern of MINUTE_PATTERNS) {
    const match = pattern.exec(message);
    if (match) return dataGapForSymbol("minutes", match[1]);
  }
  if (/^no symbol has \d+m bars in the range$/.test(message)) {
    return dataGapForSymbol("minutes", options?.fallbackSymbol);
  }
  if (options?.minutes) {
    const outgoing = OUTGOING_PATTERN.exec(message);
    if (outgoing) return dataGapForSymbol("minutes", outgoing[1]);
  }
  for (const pattern of DAILY_PATTERNS) {
    const match = pattern.exec(message);
    if (match) return dataGapForSymbol("daily", match[1]);
  }
  if (message === "no symbol has daily bars in the range") {
    return dataGapForSymbol("daily", options?.fallbackSymbol);
  }
  return null;
}
