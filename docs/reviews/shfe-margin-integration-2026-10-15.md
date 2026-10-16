# SHFE historical margin integration

Baseline: `87e2698`, primary Linux checkout, 2026-10-15.

The missing boundary is provider evidence → immutable backtest input. The existing
manual settlement events already model the account transition. They are not
provider evidence.

Two designs were considered: fetch reports during backtest submission, or use
existing table download tasks and resolve an immutable table snapshot. The latter
keeps HTTP, cancellation, request budgets, persistence and refresh in the existing
download lifecycle. Task submission reads stored facts only; workers never fetch.

First supported scope: SHFE/au/2025-12, 2025-10-20 through 2025-10-22. All six
ContractDailyTradeArgument/Settlement reports were read from the official HTTPS
site this round, including the previously missing day-start 20th and settlement
22nd. Rates are parsed from reports, never embedded in production. This is a
bounded verified source, not all-exchange historical coverage. The provider plans
only these observed report dates; it does not invent a trading calendar.

A dedicated futures.margin table records separate day-start and settlement rows.
The plugin retains both original report bodies. Data resolves a selected snapshot,
checks typed rows against retained reports and pins normalized rates, phase,
exchange-speculation basis, raw hashes, report URLs, report update labels, table
revision/content/object and provider artifact. Acquisition and report update labels
are not historical first-publication timestamps.

Terminal previews that snapshot before submission. Task resolves it independently
for the exact input days, replaces only percentage margin in the explicit daily
cost schedule and uses existing settlement events. Fees and contract specifications
remain user assumptions. Manual settlement events and provider margins cannot be
combined. Missing facts, asymmetric sides, unsupported contracts/dates/configuration
fail explicitly. Input, restart, fixed reuse, worker projection and result export
retain evidence; new data must still match the pinned margin dates. No real broker,
broker surcharge, intraday knowledge claim, forced liquidation, CSV import or
compatibility implementation is introduced. User data is not rewritten.

Official source templates:
- https://www.shfe.com.cn/data/busiparamdata/future/ContractDailyTradeArgument20251021.dat
- https://www.shfe.com.cn/data/busiparamdata/future/Settlement20251021.dat
- https://www.shfe.com.cn/images/generateData_settlement_f.js
- https://www.shfe.com.cn/publicnotice/notice/202510/t20251017_829279.html

Implementation is complete for the declared scope. Final validation is recorded below.

Integration exposed a pre-existing single-provider assumption: storage and the
transport projection accepted only Tushare evidence, and plugin deployment
recognized only the minute-history capability. Rather than repeat new vendor
allowlists in each layer, storage/transport now validate a bounded opaque format
identifier plus the existing bytes/hash envelope. Data's provider registry remains
the boundary that rejects unsupported source formats and validates their content.
The alternative (duplicate the SHFE format allowlist in storage, protocol and
registry) would preserve three policy owners. Explicit Tushare response-comparison
and refresh-observation behavior remains source-specific. Table-only capability
is now admitted by the existing plugin catalog; local and Linux package manifests
include the SHFE library.


## Final validation

- Debug build and final CTest: **678/678**, 292.02 seconds.
- Vitest: **104/104**; frontend build, lint, Prettier/clang-format and diff checks pass.
- Isolated E2E: **2/2**, `historical-margin.spec.ts` and `settlement-margin.spec.ts`:
  preview → submit → real worker → fixed reuse → parameter/result UI → evidence export.
  Market bars in these tests are fixtures, not a historical trading-performance claim.
- `pnpm desktop:check` passes with the rebuilt package and current Node-API module.
- The explicit acceptance probe loaded the **packaged Linux SHFE library**, fetched
  all six official reports through its HTTPS implementation and stored/re-read them
  in a new isolated warehouse. Provider artifact hashes match the package manifest.
  Observed fractions: 20th day-start/settlement 0.14/0.14; 21st 0.14/0.16;
  22nd 0.16/0.16. No credentials, paid API or trading counter was used.
- Ubuntu 24.04 Linux x86_64 service package: **14 files** independently hash checked,
  source fingerprint `40402f6844ede023b61330641cc372a143ce9862b1016417675c64dda7a55e60`;
  archive SHA-256 `85dd2eb449b39db283ed7761b30613373c90a28536be26b3774fbb261f0be230`.

Evidence is under `build/reviews/shfe-margin-integration-2026-10-15/`:
`verification.json`, `ctest-verified.log`, `unit-final.log`, `e2e-verified.log`,
`desktop-check.log`, `release-provider.json`, `release-provider-store/`, and
`bundle-verification.json`. The public-report probe is separate from automatic tests.
The container's final protocol client source was synchronized before CMake configure;
its packaged source fingerprint was compared with this checkout after the build.

Intermediate runs exposed the missing table-only deployment capability, an omitted
Data response case and outdated bundle fixtures; all were corrected and final
checks passed. One existing RpcHost test also reported `IPC peer disconnected`
during a run concurrent with the container build. Its implementation was not
changed; it passed the final complete run. Do not treat intermediate logs as the
final validation result.

No macOS or real desktop window was exercised. No broader historical coverage,
broker surcharge or historical first-publication claim is made. Identical normalized
partitions retain their original snapshot and original source evidence; this work
does not extend Tushare-specific same-value refresh observations to SHFE.
Each HTTPS request has a 30-second deadline; cancellation is checked at budget,
response and body-delivery boundaries. No automatic network retry is added.

Previous verified work is committed as `87e2698`. This integration is uncommitted;
no push, deployment or user-data rewrite was performed. Broader verified coverage,
factor end-to-end capacity and macOS validation remain subsequent work.
