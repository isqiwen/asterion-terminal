const axisFormatter = new Intl.DateTimeFormat("en-GB", {
  timeZone: "Asia/Shanghai",
  hourCycle: "h23",
  month: "2-digit",
  day: "2-digit",
  hour: "2-digit",
  minute: "2-digit",
  second: "2-digit",
});

// Exchange-local axis labels; the cursor retains the full localized timestamp.
export function chartTime(ns: string, includeDate: boolean, includeSeconds = false): string {
  const date = new Date(Number(BigInt(ns) / 1000000n));
  const parts = axisFormatter.formatToParts(date);
  const part = (type: Intl.DateTimeFormatPartTypes) =>
    parts.find(item => item.type === type)?.value ?? "";
  const clock = `${part("hour")}:${part("minute")}${includeSeconds ? `:${part("second")}` : ""}`;
  return includeDate ? `${part("month")}/${part("day")} ${clock}` : clock;
}
