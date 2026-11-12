import { create } from "@bufbuild/protobuf";
import { DecimalSchema, type Decimal } from "../generated/asterion/common_pb";
import { BackendError } from "../i18n/errors";

// A common.Decimal's exact text; an absent one reads as zero.
export function decimalText(value: Decimal | undefined): string {
  return value?.text || "0";
}

// An amount or a count of lots as the page shows it: thousands separated.
export function groupedText(value: Decimal | undefined): string {
  const [whole, fraction] = decimalText(value).split(".");
  const digits = whole.replace(/\B(?=(\d{3})+(?!\d))/g, ",");
  return fraction === undefined ? digits : `${digits}.${fraction}`;
}

export function decimal(text: string): Decimal {
  if (!/^-?\d+(\.\d{1,8})?$/.test(text.trim()))
    throw new BackendError("invalid_request", `invalid decimal: ${text}`);
  return create(DecimalSchema, { text: text.trim() });
}
