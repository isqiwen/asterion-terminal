export function dailyChartDate(coordinate: string): string {
  return new Date(Number(BigInt(coordinate) / 1000000n)).toISOString().slice(0, 10);
}
