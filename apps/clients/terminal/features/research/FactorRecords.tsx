import { useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { PriceChart } from "@asterion/terminal/charts/PriceChart";
import { lab } from "../../src/bridge/lab";
import { Period } from "../../src/generated/asterion/common_pb";
import type { FactorResult } from "../../src/generated/asterion/lab_pb";
import { useErrorNotification } from "../../src/host/notifications";
import { listSeparator, type MessageValues, translate } from "../../src/i18n";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import { useLibrarySort } from "./LabParts";
import { cumulativeIc, factorTurnover } from "./research-analytics";
import { recordName, recordTime } from "./research-lab";
import { saveAnnotation, type ExperimentAnnotation } from "./research-storage";
import { metric, shortTime, signed } from "./result-parts";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

const overlayColors = [
  "var(--series-1)",
  "var(--series-3)",
  "var(--series-4)",
  "var(--series-5)",
  "var(--series-2)",
];

const isComposite = (item: FactorResult) =>
  item.factorNames.length >= 2 || (item.evaluation?.extraFactors.length ?? 0) > 0;

const shortFactorName = (id: string) => id.split(".")[1] ?? id;

export const factorRecordTitle = (item: FactorResult) => {
  if (isComposite(item)) {
    const names = item.factorNames.length
      ? item.factorNames
      : [
          item.evaluation?.factor ?? "",
          ...(item.evaluation?.extraFactors.map(extra => extra.factor) ?? []),
        ].filter(Boolean);
    return names.length ? names.map(shortFactorName).join(" + ") : recordName(item.id);
  }
  const id = item.evaluation?.factor ?? "";
  return id ? shortFactorName(id) : recordName(item.id);
};

export const factorParamsLabel = (item: FactorResult) =>
  Object.entries(item.evaluation?.params ?? {})
    .map(([name, value]) => `${name}=${String(value.value.value)}`)
    .join(" ");

export const factorPeriodTag = (period: Period | undefined) => {
  switch (period) {
    case Period.MINUTES_1:
      return "1m";
    case Period.MINUTES_5:
      return "5m";
    case Period.MINUTES_10:
      return "10m";
    case Period.MINUTES_15:
      return "15m";
    case Period.MINUTES_30:
      return "30m";
    case Period.MINUTES_60:
      return "60m";
    default:
      return "";
  }
};

export function factorConditionTags(item: FactorResult): string[] {
  const ev = item.evaluation;
  const pTag = factorPeriodTag(ev?.period);
  const symbolCount = item.symbols.length || ev?.symbols.length || 0;
  return [
    pTag || t("日线"),
    symbolCount ? t("{count} 个品种", { count: symbolCount }) : "",
    ev?.returnMode === "open" ? t("次根开盘") : "",
    ev?.neutralize === "sector" ? t("板块中性") : "",
    ev?.winsorize ? t("MAD 去极值") : "",
    ev?.volAdjust ? t("波动归一") : "",
  ].filter(Boolean);
}

const closeOf = (day: string) =>
  day.includes(" ")
    ? String(BigInt(Date.parse(`${day.replace(" ", "T")}:00+08:00`)) * 1_000_000n)
    : String(BigInt(Date.parse(`${day}T07:00:00Z`)) * 1_000_000n);

type SortKey = "ic" | "holdoutIc" | "ir" | "t" | "turnover" | "evaluated";

const figuresOf = (item: FactorResult) => {
  const first = item.horizons[0];
  return {
    ic: first?.mean,
    holdoutIc: item.holdoutHorizons[0]?.mean,
    ir: first?.ir,
    t: first?.t,
    turnover: factorTurnover(item),
    evaluated: Number(item.evaluatedMs),
  };
};

export function FactorRecords({
  records,
  annotations,
  search,
  setSearch,
  onScope,
  busy,
  onOpen,
  onCompare,
  onDeleteMany,
  switcher,
  notices,
}: {
  records: readonly FactorResult[];
  annotations: Record<string, ExperimentAnnotation>;
  search: string;
  setSearch: (value: string) => void;
  onScope?: (result: FactorResult) => boolean;
  busy: boolean;
  onOpen: (result: FactorResult) => void;
  onCompare: (ids: string[]) => void;
  onDeleteMany: (ids: string[]) => Promise<void>;
  switcher: ReactNode;
  notices: ReactNode;
}) {
  const { heading, sorted, status } = useLibrarySort<SortKey>("evaluated");
  const [kindFilter, setKindFilter] = useState<"" | "single" | "composite">("");
  const [onlyScope, setOnlyScope] = useState(false);
  const [onlyStarred, setOnlyStarred] = useState(false);
  const [selected, setSelected] = useState<string[]>([]);
  const [confirmAction, setConfirmAction] = useState<"selected" | "unstarred" | null>(null);
  const [deleting, setDeleting] = useState(false);
  const selectAllRef = useRef<HTMLInputElement>(null);

  const validIds = useMemo(() => new Set(records.map(item => item.id)), [records]);
  const activeSelected = useMemo(
    () => selected.filter(id => validIds.has(id)),
    [selected, validIds],
  );
  const query = search.trim().toLowerCase();

  const filtered = useMemo(
    () =>
      records.filter(item => {
        const composite = isComposite(item);
        if (kindFilter === "single" && composite) return false;
        if (kindFilter === "composite" && !composite) return false;
        if (onlyScope && onScope && !onScope(item)) return false;
        const ann = annotations[item.id];
        if (onlyStarred && !ann?.starred) return false;
        if (!query) return true;
        const haystack = [
          item.id,
          item.evaluation?.factor ?? "",
          ...item.factorNames,
          factorParamsLabel(item),
          item.evaluation?.firstDay ?? "",
          item.evaluation?.lastDay ?? "",
          ...(item.symbols.length ? item.symbols : (item.evaluation?.symbols ?? [])),
          ann?.note ?? "",
        ]
          .join(" ")
          .toLowerCase();
        return haystack.includes(query);
      }),
    [records, kindFilter, onlyScope, onScope, onlyStarred, annotations, query],
  );

  const rows = sorted(filtered, (item, key) => figuresOf(item)[key]);
  const shownIds = rows.map(item => item.id);
  const selectedShown = shownIds.filter(id => activeSelected.includes(id));
  const allShown = shownIds.length > 0 && selectedShown.length === shownIds.length;
  const someShown = selectedShown.length > 0 && !allShown;
  const unstarredShown = useMemo(
    () => rows.filter(item => !annotations[item.id]?.starred).map(item => item.id),
    [rows, annotations],
  );

  useEffect(() => {
    if (selectAllRef.current) selectAllRef.current.indeterminate = someShown;
  }, [someShown]);

  useEffect(() => {
    setConfirmAction(null);
  }, [activeSelected.length, kindFilter, onlyScope, onlyStarred, query, records.length]);

  const toggleOne = (id: string, checked: boolean) =>
    setSelected(previous =>
      checked
        ? previous.includes(id)
          ? previous
          : [...previous, id]
        : previous.filter(item => item !== id),
    );
  const toggleAll = (checked: boolean) =>
    setSelected(previous =>
      checked
        ? [...new Set([...previous, ...shownIds])]
        : previous.filter(id => !shownIds.includes(id)),
    );

  const runDelete = async (ids: string[]) => {
    if (!ids.length) return;
    setDeleting(true);
    setConfirmAction(null);
    await onDeleteMany(ids);
    setSelected(previous => previous.filter(id => !ids.includes(id)));
    setDeleting(false);
  };

  return (
    <div className="lab-center lab-library-view">
      <div className="lab-toolbar">
        {switcher}
        <input
          type="search"
          aria-label={t("搜索评价记录")}
          placeholder={t("搜索评价记录")}
          spellCheck={false}
          autoCorrect="off"
          autoCapitalize="none"
          value={search}
          onChange={event => setSearch(event.target.value)}
        />
        <label className="lab-inline">
          <span>{t("类型")}</span>
          <select
            aria-label={t("记录类型")}
            value={kindFilter}
            onChange={event => setKindFilter(event.target.value as "" | "single" | "composite")}
          >
            <option value="">{t("全部")}</option>
            <option value="single">{t("单因子")}</option>
            <option value="composite">{t("多因子合成")}</option>
          </select>
        </label>
        {onScope && (
          <label className="lab-inline lab-check-inline">
            <input
              type="checkbox"
              checked={onlyScope}
              onChange={event => setOnlyScope(event.target.checked)}
            />
            <span>{t("仅当前条件")}</span>
          </label>
        )}
        <label className="lab-inline lab-check-inline">
          <input
            type="checkbox"
            checked={onlyStarred}
            onChange={event => setOnlyStarred(event.target.checked)}
          />
          <span>{t("仅看星标")}</span>
        </label>
        <span className="lab-spacer" />
        {activeSelected.length > 0 && (
          <button
            type="button"
            className="lab-link"
            disabled={busy || deleting}
            onClick={() => setSelected([])}
          >
            {t("清空选择")}
          </button>
        )}
        {activeSelected.length >= 2 && (
          <button
            type="button"
            disabled={busy || deleting || activeSelected.length > 5}
            title={activeSelected.length > 5 ? t("一次最多比较 5 条记录") : undefined}
            onClick={() => onCompare(activeSelected)}
          >
            {t("比较（{count}）", { count: activeSelected.length })}
          </button>
        )}
        {activeSelected.length > 0 ? (
          confirmAction === "selected" ? (
            <>
              <button
                type="button"
                className="danger"
                disabled={busy || deleting}
                onClick={() => void runDelete(activeSelected)}
              >
                {t("确认删除（{count}）", { count: activeSelected.length })}
              </button>
              <button
                type="button"
                className="lab-link"
                disabled={deleting}
                onClick={() => setConfirmAction(null)}
              >
                {t("取消")}
              </button>
            </>
          ) : (
            <button
              type="button"
              className="danger-quiet"
              disabled={busy || deleting}
              onClick={() => setConfirmAction("selected")}
            >
              {t("删除所选（{count}）", { count: activeSelected.length })}
            </button>
          )
        ) : (
          unstarredShown.length > 0 &&
          (confirmAction === "unstarred" ? (
            <>
              <button
                type="button"
                className="danger"
                disabled={busy || deleting}
                onClick={() => void runDelete(unstarredShown)}
              >
                {t("确认清理（{count}）", { count: unstarredShown.length })}
              </button>
              <button
                type="button"
                className="lab-link"
                disabled={deleting}
                onClick={() => setConfirmAction(null)}
              >
                {t("取消")}
              </button>
            </>
          ) : (
            <button
              type="button"
              className="danger-quiet"
              disabled={busy || deleting}
              onClick={() => setConfirmAction("unstarred")}
            >
              {t("清理未星标（{count}）", { count: unstarredShown.length })}
            </button>
          ))
        )}
      </div>
      {notices}
      {status}
      {rows.length > 0 ? (
        <div
          className="lab-scroll lab-library-scroll"
          tabIndex={0}
          role="region"
          aria-label={t("评价记录")}
        >
          <table className="lab-table lab-records lab-library-table" aria-label={t("评价记录")}>
            <thead>
              <tr>
                <th scope="col" className="lab-check-col">
                  <input
                    ref={selectAllRef}
                    type="checkbox"
                    aria-label={t("全选记录")}
                    checked={allShown}
                    disabled={shownIds.length === 0}
                    onChange={event => toggleAll(event.target.checked)}
                  />
                </th>
                <th scope="col" className="lab-star-col">
                  <span className="sr-only">{t("加星标")}</span>
                </th>
                <th scope="col" className="lab-name-col">
                  {t("因子")}
                </th>
                <th scope="col">{t("评价条件")}</th>
                {heading("ic", t("IC 均值"), t("第一个持有期"))}
                {heading("holdoutIc", t("留出期 IC"), t("留出期第一个持有期"))}
                {heading("ir", "ICIR")}
                {heading("t", t("t 值"))}
                {heading("turnover", t("换手率"), t("各分组日均换手率均值"))}
                {heading("evaluated", t("评价时间"))}
              </tr>
            </thead>
            <tbody>
              {rows.map(item => {
                const checked = activeSelected.includes(item.id);
                const offScope = Boolean(onScope && !onScope(item));
                const ann = annotations[item.id];
                const paramsText = factorParamsLabel(item);
                const condTags = factorConditionTags(item);
                const span =
                  item.evaluation?.firstDay && item.evaluation?.lastDay
                    ? `${item.evaluation.firstDay} – ${item.evaluation.lastDay}`
                    : "";
                const values = figuresOf(item);
                const title = factorRecordTitle(item);
                return (
                  <tr
                    key={item.id}
                    className={checked ? "selected" : undefined}
                    data-empty={offScope || undefined}
                    title={ann?.note || item.id}
                    onClick={() => onOpen(item)}
                  >
                    <td className="lab-check-col" onClick={event => event.stopPropagation()}>
                      <input
                        type="checkbox"
                        aria-label={t("选择 {name}", { name: `${title} ${recordTime(item.id)}` })}
                        checked={checked}
                        onChange={event => toggleOne(item.id, event.target.checked)}
                      />
                    </td>
                    <td className="lab-star-col" onClick={event => event.stopPropagation()}>
                      <button
                        type="button"
                        className={`lab-star-btn${ann?.starred ? " is-starred" : ""}`}
                        aria-label={ann?.starred ? t("取消星标") : t("加星标")}
                        aria-pressed={Boolean(ann?.starred)}
                        onClick={() => saveAnnotation(item.id, { starred: !ann?.starred })}
                      >
                        {ann?.starred ? "★" : "☆"}
                      </button>
                    </td>
                    <th scope="row">
                      <button
                        type="button"
                        className="lab-link strong"
                        onClick={event => {
                          event.stopPropagation();
                          onOpen(item);
                        }}
                      >
                        {title}
                      </button>
                      {paramsText && <span className="lab-record-params">{paramsText}</span>}
                      {ann?.note && <span className="lab-record-note">{ann.note}</span>}
                    </th>
                    <td className="lab-desc subtle">
                      {[...condTags, span, offScope ? t("条件不同") : ""]
                        .filter(Boolean)
                        .join(" · ")}
                    </td>
                    <td className={`numeric ${signed(values.ic)}`}>{metric(values.ic, false)}</td>
                    <td className={`numeric ${signed(values.holdoutIc)}`}>
                      {metric(values.holdoutIc, false)}
                    </td>
                    <td className="numeric">{metric(values.ir, false)}</td>
                    <td className="numeric">{metric(values.t, false)}</td>
                    <td className="numeric">{metric(values.turnover, true)}</td>
                    <td className="subtle numeric">{shortTime(item.evaluatedMs)}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      ) : (
        <p className="subtle lab-records-empty">
          {records.length === 0 ? t("还没有评价记录") : t("没有匹配的评价记录")}
        </p>
      )}
    </div>
  );
}

export function FactorComparison({
  ids,
  onBack,
  onOpen,
}: {
  ids: readonly string[];
  onBack: () => void;
  onOpen: (result: FactorResult) => void;
}) {
  return (
    <FactorComparisonBody
      key={ids.join("\n")}
      joined={ids.join("\n")}
      onBack={onBack}
      onOpen={onOpen}
    />
  );
}

function FactorComparisonBody({
  joined,
  onBack,
  onOpen,
}: {
  joined: string;
  onBack: () => void;
  onOpen: (result: FactorResult) => void;
}) {
  const ids = useMemo(() => joined.split("\n").filter(Boolean), [joined]);
  const [results, setResults] = useState<FactorResult[]>([]);
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    void Promise.all(ids.map(id => lab({ case: "getFactorResult", value: { id } })))
      .then(replies => active && setResults(replies))
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [ids]);
  useErrorNotification(error, "asterion.terminal.research");

  const seriesColor = (index: number) =>
    index === 0 ? "var(--price-line)" : overlayColors[(index - 1) % overlayColors.length]!;

  const chartData = useMemo(() => {
    if (results.length < 2) return null;
    const curves = results.map(item => {
      const cum = cumulativeIc(item.days);
      const map = new Map<string, number>();
      item.days.forEach((day, idx) => {
        if (day.ic !== undefined) map.set(day.tradingDay, cum[idx]!);
      });
      return map;
    });
    const baseDays = results[0]!.days.filter(day => day.ic !== undefined);
    if (!baseDays.length) return null;
    const firstPoints = baseDays.map(day => ({
      timestamp_ns: closeOf(day.tradingDay),
      price: (curves[0]!.get(day.tradingDay) ?? 0).toFixed(4),
    }));
    const overlays = results.slice(1).map((item, idx) => ({
      label: `${factorRecordTitle(item)} ${recordTime(item.id)}`,
      color: seriesColor(idx + 1),
      values: baseDays.map(day => curves[idx + 1]!.get(day.tradingDay) ?? null),
    }));
    const intraday = baseDays.some(day => day.tradingDay.includes(" "));
    return { baseDays, firstPoints, overlays, intraday };
  }, [results]);

  return (
    <div className="lab-center lab-runs-view">
      <header className="lab-report-header">
        <button type="button" className="lab-link" onClick={onBack}>
          {t("← 评价记录")}
        </button>
        <h2>{t("比较 {count} 条评价记录", { count: ids.length })}</h2>
      </header>
      {error ? null : results.length !== ids.length ? (
        <p className="subtle">{t("正在读取结果…")}</p>
      ) : (
        <div className="lab-result">
          <div className="lab-scroll">
            <table className="lab-table lab-compare" aria-label={t("评价记录对比")}>
              <thead>
                <tr>
                  <th scope="col">
                    <span className="sr-only">{t("指标")}</span>
                  </th>
                  {results.map((item, index) => {
                    const params = factorParamsLabel(item);
                    const label = `${factorRecordTitle(item)}${params ? ` · ${params}` : ""}`;
                    return (
                      <th scope="col" key={item.id} className="numeric">
                        <span
                          className="lab-swatch"
                          aria-hidden="true"
                          style={{ background: seriesColor(index) }}
                        />
                        <button
                          type="button"
                          className="lab-link strong"
                          onClick={() => onOpen(item)}
                        >
                          {label}
                        </button>
                        <div className="subtle">{recordTime(item.id)}</div>
                      </th>
                    );
                  })}
                </tr>
              </thead>
              <tbody>
                {(
                  [
                    [
                      t("IC 均值"),
                      (r: FactorResult) => metric(r.horizons[0]?.mean, false),
                      (r: FactorResult) => signed(r.horizons[0]?.mean),
                    ],
                    [
                      t("IC 标准差"),
                      (r: FactorResult) => metric(r.horizons[0]?.deviation, false),
                      () => "",
                    ],
                    ["ICIR", (r: FactorResult) => metric(r.horizons[0]?.ir, false), () => ""],
                    [t("t 值"), (r: FactorResult) => metric(r.horizons[0]?.t, false), () => ""],
                    [
                      t("HAC t 值"),
                      (r: FactorResult) => metric(r.horizons[0]?.tHac, false),
                      () => "",
                    ],
                    [
                      t("IC 为正"),
                      (r: FactorResult) => metric(r.horizons[0]?.positiveShare, true),
                      () => "",
                    ],
                    [
                      t("留出期 IC"),
                      (r: FactorResult) => metric(r.holdoutHorizons[0]?.mean, false),
                      (r: FactorResult) => signed(r.holdoutHorizons[0]?.mean),
                    ],
                    [
                      t("留出期 ICIR"),
                      (r: FactorResult) => metric(r.holdoutHorizons[0]?.ir, false),
                      () => "",
                    ],
                    [t("换手率"), (r: FactorResult) => metric(factorTurnover(r), true), () => ""],
                    [t("信号自相关"), (r: FactorResult) => metric(r.autocorr, false), () => ""],
                    [
                      t("信号半衰期"),
                      (r: FactorResult) =>
                        r.halfLife === undefined
                          ? "—"
                          : t("{bars} 根", { bars: r.halfLife.toFixed(1) }),
                      () => "",
                    ],
                    [
                      t("多空累计"),
                      (r: FactorResult) => {
                        const last = r.days.at(-1)?.longShort;
                        return last === undefined ? "—" : metric(last - 1, true);
                      },
                      (r: FactorResult) => {
                        const last = r.days.at(-1)?.longShort;
                        return last === undefined ? "" : signed(last - 1);
                      },
                    ],
                    [
                      t("扣费多空累计"),
                      (r: FactorResult) => {
                        const last = r.days.at(-1)?.netLongShort;
                        return last === undefined ? "—" : metric(last - 1, true);
                      },
                      (r: FactorResult) => {
                        const last = r.days.at(-1)?.netLongShort;
                        return last === undefined ? "" : signed(last - 1);
                      },
                    ],
                  ] as const
                ).map(([label, read, cls]) => (
                  <tr key={label}>
                    <td>{label}</td>
                    {results.map(item => (
                      <td key={item.id} className={`numeric ${cls(item)}`}>
                        {read(item)}
                      </td>
                    ))}
                  </tr>
                ))}
                {(
                  [
                    [
                      t("因子"),
                      (r: FactorResult) =>
                        isComposite(r) ? r.factorNames.join(" + ") : (r.evaluation?.factor ?? ""),
                    ],
                    [t("参数"), (r: FactorResult) => factorParamsLabel(r) || "—"],
                    [
                      t("周期"),
                      (r: FactorResult) => factorPeriodTag(r.evaluation?.period) || t("日线"),
                    ],
                    [
                      t("品种"),
                      (r: FactorResult) => t("{count} 个品种", { count: r.symbols.length }),
                    ],
                    [
                      t("区间"),
                      (r: FactorResult) =>
                        `${r.evaluation?.firstDay ?? ""} – ${r.evaluation?.lastDay ?? ""}`,
                    ],
                    [
                      t("持有期"),
                      (r: FactorResult) => (r.evaluation?.horizons ?? []).join(listSeparator()),
                    ],
                    [t("分组"), (r: FactorResult) => String(r.evaluation?.groups ?? "")],
                    [
                      t("收益口径"),
                      (r: FactorResult) =>
                        r.evaluation?.returnMode === "open" ? t("次根开盘") : t("收盘对收盘"),
                    ],
                    [
                      t("中性化"),
                      (r: FactorResult) =>
                        r.evaluation?.neutralize === "sector" ? t("板块中性化") : t("不中性化"),
                    ],
                    [t("留出日"), (r: FactorResult) => r.evaluation?.holdoutDay || "—"],
                    [
                      t("研究数据"),
                      (r: FactorResult) => r.pinned?.datasetVersion.slice(0, 8) || "—",
                    ],
                  ] as const
                )
                  .filter(([, read]) => new Set(results.map(read)).size > 1)
                  .map(([label, read]) => (
                    <tr key={label} className="lab-compare-diff">
                      <td>{label}</td>
                      {results.map(item => (
                        <td key={item.id} className="numeric subtle">
                          {read(item)}
                        </td>
                      ))}
                    </tr>
                  ))}
              </tbody>
            </table>
          </div>
          {chartData && (
            <>
              <h3>{t("累计 IC 对比")}</h3>
              <PriceChart
                series={{ kind: "line", points: chartData.firstPoints }}
                overlays={chartData.overlays}
                label={t("累计 IC")}
                titled
                volumeLabel={t("成交量")}
                showVolume={false}
                formatTime={ns => {
                  const iso = new Date(
                    Number(BigInt(ns) / 1_000_000n) + 8 * 3600_000,
                  ).toISOString();
                  return chartData.intraday
                    ? `${iso.slice(5, 10)} ${iso.slice(11, 16)}`
                    : iso.slice(0, 10);
                }}
                describe={index =>
                  chartData.baseDays[index]
                    ? `${chartData.baseDays[index].tradingDay} · ${chartData.firstPoints[index]?.price ?? ""}`
                    : null
                }
              />
            </>
          )}
        </div>
      )}
    </div>
  );
}
