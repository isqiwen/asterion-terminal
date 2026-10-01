# Chart indicators

Display-only numerical helpers for service-owned datasets. No order, ledger or research API consumes these floating-point values. Raw bars remain Decimal.

MACD uses EMA periods 12 and 26, signal period 9, and histogram `2 * (diff - signal)`. Price EMAs are seeded with the first close of the complete dataset; signal starts at zero. The first 33 observations are withheld. Queries must replay the complete prefix, including observations before a time filter, before emitting values. No page-local seeding is allowed.

The EMA smoothing factors follow the standard `2 / (period + 1)` recurrence; [TA-Lib's primary implementation](https://github.com/TA-Lib/ta-lib/blob/main/src/ta_func/ta_MACD.c) is a reference for the formula, not an equivalence claim. TA-Lib's default SMA seeding and histogram convention differ from this explicitly defined display convention.

`quote_volume` computes an exact integer difference only for nonnegative, monotonic cumulative counters within a known trading day. A first observation, missing trading day, negative counter or counter reset has unknown interval volume, not zero. This is observed quote-interval volume, not tick-trade size.

`calendar_bars` groups strictly ordered, unique daily trading dates into ISO Monday–Sunday weeks or calendar months, quarters and years. It keeps the first open/reference prices, maximum high, minimum low, last close/settlement/open interest, and exact Decimal sums of volume/amount. A missing final settlement clears the aggregate settlement. The label is the last observed source date. No empty dates or periods are invented; calendar bucket bounds and source-row counts do not establish exchange-session completeness. Checked Decimal overflow fails the query instead of returning truncated values.

The data pipeline aggregates the complete verified daily dataset before applying label-date filters or page offsets. MACD then consumes aggregated closes from the dataset origin. Its 33-record warm-up therefore means 33 bars of the selected aggregate period for those periods, not 33 daily observations. This is chart data, not an executable intrabar path or a substitute for exchange calendars.
