import { create } from "@bufbuild/protobuf";
import { DecimalSchema, type Decimal } from "../generated/asterion/domain_pb";
import { BackendError } from "../i18n/errors";

// asterion::Decimal: a signed count of 1e-8 units.
const scale = 100_000_000n;

export function decimalText(value: Decimal | undefined): string {
  const units = value?.units ?? 0n;
  const magnitude = units < 0n ? -units : units;
  const fraction = (magnitude % scale).toString().padStart(8, "0").replace(/0+$/, "");
  return `${units < 0n ? "-" : ""}${magnitude / scale}${fraction ? `.${fraction}` : ""}`;
}

export function decimal(text: string): Decimal {
  const match = /^(-?)(\d+)(?:\.(\d{1,8}))?$/.exec(text.trim());
  if (!match) throw new BackendError("invalid_request", `invalid decimal: ${text}`);
  const units = BigInt(match[2]!) * scale + BigInt((match[3] ?? "").padEnd(8, "0"));
  return create(DecimalSchema, { units: match[1] ? -units : units });
}
