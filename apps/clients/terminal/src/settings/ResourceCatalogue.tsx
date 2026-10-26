import { useEffect, useRef, useState } from "react";
import {
  request,
  type Snapshot,
  type ResourceCatalogue as Catalogue,
  type ResourceEntry,
  type ResourceInspection,
} from "../bridge/client";
import { translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
const t = (key: string) => translate("host", key);
const kinds = {
  data: [
    "minute_version",
    "table_snapshot",
    "provider_module",
    "observation_archive",
    "material_acquisition",
    "material_content",
  ],
  research: [
    "backtest",
    "backtest_result",
    "factor",
    "factor_result",
    "statistics",
    "statistics_result",
    "performance_evaluation",
    "performance_evaluation_result",
    "sample",
    "sample_result",
    "account_review",
    "account_review_result",
    "research_program",
    "research_project",
    "research_plan",
    "research_conclusion",
    "research_variable",
    "logic_definition",
    "logic_case",
    "logic_trial",
    "logic_trial_result",
    "strategy_package",
    "strategy_delivery",
  ],
};
const maintenanceNames: Record<string, string> = {
  available: "可进入所属域确认",
  blocked: "存在引用或活动，禁止退役",
  pending: "封存清理中",
  removed: "已退役/已清理",
  retired: "已退役（证据保留）",
};
const names: Record<string, string> = {
  minute_version: "分钟版本",
  table_snapshot: "表快照",
  provider_module: "供应商程序",
  observation_archive: "观察归档",
  material_acquisition: "材料取得记录",
  material_content: "材料固定原件",
  backtest: "回测固定输入",
  backtest_result: "回测正式结果",
  factor: "因子固定输入",
  factor_result: "因子正式结果",
  statistics: "统计固定输入",
  statistics_result: "统计正式报告",
  performance_evaluation: "绩效评价固定输入",
  performance_evaluation_result: "绩效评价正式报告",
  sample: "样本固定输入",
  sample_result: "样本正式证据",
  account_review: "账户审阅固定输入",
  account_review_result: "账户审阅正式结果",
  research_program: "研究执行程序原件",
  research_project: "研究项目草稿",
  research_plan: "研究固定假设计划",
  research_conclusion: "研究人工结论",
  research_variable: "变量",
  logic_definition: "独立逻辑定义",
  logic_case: "事先虚构样例",
  logic_trial: "固定逻辑试算",
  logic_trial_result: "逻辑试算正式证据",
  strategy_package: "固定策略候选",
  strategy_delivery: "策略交付草稿",
};
export function ResourceCatalogue({ snapshot }: { snapshot: Snapshot | null }) {
  const [domain, setDomain] = useState<"data" | "research">("data");
  const [kind, setKind] = useState("minute_version");
  const [catalogue, setCatalogue] = useState<Catalogue | null>(null);
  const [inspection, setInspection] = useState<ResourceInspection | null>(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const epoch = useRef({ value: 0 });
  const source = domain === "data" ? snapshot?.data : snapshot?.research_service;
  const connection = source?.connection_id;
  useEffect(() => {
    ++epoch.current.value;
    setCatalogue(null);
    setInspection(null);
    setPending(false);
    setError("");
    const token = epoch.current;
    return () => {
      ++token.value;
    };
  }, [connection, domain, kind]);
  const root = snapshot?.nodes
    .find(node => node.id === source?.node_id)
    ?.health?.services.find(service => service.id === source?.service)?.directory;
  async function load(after = "") {
    const current = ++epoch.current.value;
    setPending(true);
    setError("");
    setInspection(null);
    try {
      const reply = await request("resource.catalogue", { domain, kind, after });
      if (current !== epoch.current.value) return;
      if (
        !reply.resources ||
        reply.resources.kind !== kind ||
        reply.resources.owner.domain !== domain ||
        reply.resources.owner.service !== source?.service
      )
        throw new Error(t("资产目录所有者或范围不符，请重新读取"));
      const page = reply.resources;
      setCatalogue(previous =>
        after && previous ? { ...page, items: [...previous.items, ...page.items] } : page,
      );
    } catch (reason) {
      if (current === epoch.current.value) {
        setCatalogue(null);
        setError(asDisplayError(reason));
      }
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  async function inspect(entry: ResourceEntry) {
    const current = ++epoch.current.value;
    setPending(true);
    setError("");
    setInspection(null);
    try {
      const reply = await request("resource.inspect", { object: entry.object });
      if (current !== epoch.current.value) return;
      const value = reply.resource_inspection;
      if (
        !value ||
        (["node", "service", "domain", "kind", "id", "version"] as const).some(
          key => value.object[key] !== entry.object[key],
        )
      )
        throw new Error(t("资产预览身份不符，请重新读取"));
      setInspection(value);
    } catch (reason) {
      if (current === epoch.current.value) setError(asDisplayError(reason));
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  const blocked = pending || !source?.online;
  return (
    <section className="settings-section resource-catalogue" aria-label={t("业务资产目录")}>
      <p>
        {t(
          "从当前明确选择的业务服务读取固定资产身份。目录可重建，不授权删除，也不证明载荷全部可访问。",
        )}
      </p>
      <p>
        {t(
          "当前支持 Data 固定数据/材料/保存内容、Intelligence 候选/准备/发布与程序、Research 项目/计划/人工结论与固定输入/结果；目录不授权删除。",
        )}
      </p>
      <label>
        {t("资产所属域")}
        <select
          disabled={pending}
          value={domain}
          onChange={event => {
            const value = event.target.value as "data" | "research";
            setDomain(value);
            setKind(kinds[value][0]);
          }}
        >
          <option value="data">Data</option>
          <option value="research">Research</option>
        </select>
      </label>
      <label>
        {t("资产类别")}
        <select disabled={pending} value={kind} onChange={event => setKind(event.target.value)}>
          {kinds[domain].map(value => (
            <option key={value} value={value}>
              {t(names[value])}
            </option>
          ))}
        </select>
      </label>
      <p>
        {t("当前服务")}:{" "}
        <code>
          {source?.node_id ?? "—"} / {source?.service ?? "—"}
        </code>
      </p>
      {!source?.online && (
        <p role="status">{t("所选所有者不可达，目录不能作为当前可用性证明。")}</p>
      )}
      <button disabled={blocked} onClick={() => void load()}>
        {t("读取资产目录")}
      </button>
      {pending && <p role="status">{t("正在读取所属域资产…")}</p>}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {catalogue && (
        <>
          <p>
            {t("稳定所有者")}:{" "}
            <code>
              {catalogue.owner.node} / {catalogue.owner.service} / {catalogue.owner.domain}
            </code>
          </p>
          <p>{t("逻辑载荷字节不等于磁盘独占占用，不能按目录行相加。未知字节不填零。")}</p>
          {!catalogue.items.length && <p>{t("该范围没有已登记资产")}</p>}
          <table aria-label={t("所属域资产")}>
            <thead>
              <tr>
                <th>{t("身份与固定版本")}</th>
                <th>{t("生命周期")}</th>
                <th>{t("逻辑载荷字节")}</th>
                <th>{t("最近观测的保存位置")}</th>
                <th>{t("核对")}</th>
              </tr>
            </thead>
            <tbody>
              {catalogue.items.map(entry => (
                <tr key={`${entry.object.id}/${entry.object.version}`}>
                  <td>
                    {entry.name && <strong>{entry.name}</strong>}
                    <code>{entry.object.id}</code>
                    <br />
                    <code>{entry.object.version}</code>
                  </td>
                  <td>
                    {t(
                      entry.object.kind === "research_project"
                        ? "研究草稿（可修订）"
                        : (entry.object.kind === "research_plan" ||
                              entry.object.kind === "research_conclusion" ||
                              entry.object.kind === "research_variable" ||
                              entry.object.kind === "strategy_package" ||
                              entry.object.kind === "logic_definition" ||
                              entry.object.kind === "logic_case" ||
                              entry.object.kind === "strategy_delivery") &&
                            entry.state === "sealed"
                          ? "已固定（只读）"
                          : entry.state === "registered"
                            ? "已登记（载荷未核验）"
                            : entry.state === "sealed"
                              ? "封存清理中"
                              : "已退役",
                    )}
                  </td>
                  <td>{entry.logical_bytes ?? t("未知")}</td>
                  <td>
                    <code>
                      {root ? `${root}/${entry.relative_location}` : entry.relative_location}
                    </code>
                  </td>
                  <td>
                    {entry.object.kind === "provider_module" ||
                    entry.object.kind === "research_program" ? (
                      <span>{t("程序工件维护尚未实现，不提供删除操作。")}</span>
                    ) : entry.object.kind === "logic_definition" ||
                      entry.object.kind === "logic_case" ? (
                      <span>
                        {t("在策略逻辑入口读取原规则和事先预期；此目录不提供修改或删除。")}
                      </span>
                    ) : entry.object.kind === "strategy_package" ||
                      entry.object.kind === "strategy_delivery" ? (
                      <span>
                        {t("在策略管理与交付入口读取原定义和任务状态；此目录不提供修改或删除。")}
                      </span>
                    ) : [
                        "research_project",
                        "research_plan",
                        "research_conclusion",
                        "research_variable",
                      ].includes(entry.object.kind) ? (
                      <span>
                        {t(
                          entry.object.kind === "research_variable"
                            ? "在研究变量入口读取原定义；此目录不提供修改或删除。"
                            : "在研究项目入口读取原计划与人工结论；此目录不提供修改或删除。",
                        )}
                      </span>
                    ) : entry.object.kind === "material_acquisition" ||
                      entry.object.kind === "material_content" ? (
                      <span>{t("在数据材料入口读取原件；材料删除尚未实现。")}</span>
                    ) : entry.object.kind === "observation_archive" ? (
                      <span>
                        {t("名称为归档操作 ID；在 Data 历史数据仓库读取原件，不提供删除。")}
                      </span>
                    ) : (
                      <button disabled={blocked} onClick={() => void inspect(entry)}>
                        {t("查看引用与维护")}
                      </button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          {catalogue.next_after && (
            <button disabled={blocked} onClick={() => void load(catalogue.next_after)}>
              {t("继续读取资产")}
            </button>
          )}
        </>
      )}
      {inspection && (
        <section aria-label={t("资源引用与维护预览")}>
          <code>
            {inspection.object.id} / {inspection.object.version}
          </code>
          <p>
            {t("所属域维护状态")}:{" "}
            {t(maintenanceNames[inspection.maintenance.state] ?? "维护状态未知")}
          </p>
          {inspection.maintenance.current_head && <p>{t("当前头快照不能退役")}</p>}
          {inspection.maintenance.selected && (
            <p>{t("本窗口草稿正在使用此资产，业务维护入口会阻止删除。")}</p>
          )}
          {inspection.maintenance.active && <p>{t("任务仍活动，不能退役")}</p>}
          <ul>
            {inspection.maintenance.acquisitions?.map(id => (
              <li key={id}>
                <code>{id}</code>
              </li>
            ))}
            {inspection.maintenance.references?.map(ref => (
              <li key={ref.id}>{ref.name || ref.id}</li>
            ))}
            {inspection.maintenance.consumers.map(ref => (
              <li key={typeof ref === "string" ? ref : `${ref.node}/${ref.service}/${ref.id}`}>
                <code>
                  {typeof ref === "string" ? ref : `${ref.node}/${ref.service}/${ref.id}`}
                </code>
              </li>
            ))}
          </ul>
          <p>{t("此页只读。退役、清理与恢复仍需在所属业务维护入口核对并明确确认。")}</p>
        </section>
      )}
    </section>
  );
}
