import { useState } from "react";
import type { DataConnection, Snapshot, TerminalCommand } from "../bridge/client";
import { translate, useLocale } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
const t = (key: string) => translate("host", key);
const checkStates = [
  "接口访问成功",
  "尚未验证",
  "凭据无效",
  "接口权限不足",
  "请求受到限流",
  "网络连接失败",
  "验证失败",
];
export function DataConnections({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
}) {
  const { locale } = useLocale();
  const sources = snapshot?.research?.sources.filter(source => source.connection) ?? [];
  const connections = snapshot?.data_connections ?? [];
  const [editing, setEditing] = useState<DataConnection | null>(null);
  const [sourceId, setSourceId] = useState("");
  const [name, setName] = useState("");
  const [credential, setCredential] = useState("");
  const [remember, setRemember] = useState(false);
  const [rate, setRate] = useState("60");
  const [error, setError] = useState<DisplayError>("");
  const [removing, setRemoving] = useState("");
  const source = sources.find(item => item.id === sourceId);
  const schema = source?.connection;
  const run = async (method: TerminalCommand, params?: Record<string, unknown>) => {
    setError("");
    try {
      await trade(method, params);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  };
  const edit = (connection: DataConnection | null) => {
    setEditing(connection);
    setCredential("");
    setError("");
    setRemoving("");
    setSourceId(connection?.source ?? "");
    setName(connection?.name ?? "");
    setRemember(connection?.remember ?? false);
    setRate(String(connection?.requests_per_minute ?? 60));
  };
  return (
    <section className="native-plugin-service" aria-label={t("数据源连接")}>
      <h2>{t("数据源连接")}</h2>
      <p>{t("连接保存凭据策略与请求上限；合约、日期和周期由下载任务选择。")}</p>
      {!snapshot?.research?.online && (
        <button disabled={busy} onClick={() => void run("research.local")}>
          {t("连接本机研究服务")}
        </button>
      )}
      {connections.map(connection => {
        const provider = sources.find(
          item => item.id === connection.source && item.plugin_id === connection.plugin_id,
        );
        const verification = snapshot?.connection_verification;
        return (
          <section key={connection.id} aria-label={connection.name}>
            <h3>{connection.name}</h3>
            <p>
              {connection.source} ·{" "}
              {connection.remember ? t("凭据已保存到本机") : t("凭据仅在本次运行中使用")}
            </p>
            {!provider ? (
              <p>{t("数据源插件未启用")}</p>
            ) : provider.connection?.credential_required && !connection.credential_ready ? (
              <p>{t("需要重新输入凭据")}</p>
            ) : null}
            <button disabled={busy} onClick={() => edit(connection)}>
              {t("编辑连接")}
            </button>{" "}
            <button
              disabled={
                busy ||
                !snapshot?.research?.online ||
                !provider ||
                (!!provider.connection?.credential_required && !connection.credential_ready)
              }
              onClick={() =>
                void run("research.connections.verify", {
                  id: connection.id,
                  revision: connection.revision,
                  source: connection.source,
                })
              }
            >
              {t("验证连接")}
            </button>{" "}
            {removing === connection.id ? (
              <button
                disabled={busy}
                onClick={async () => {
                  if (
                    await run("research.connections.remove", {
                      id: connection.id,
                      revision: connection.revision,
                    })
                  )
                    edit(null);
                }}
              >
                {t("确认删除连接")}
              </button>
            ) : (
              <button disabled={busy} onClick={() => setRemoving(connection.id)}>
                {t("删除连接")}
              </button>
            )}
            {verification?.id === connection.id &&
              verification.revision === connection.revision && (
                <ul>
                  {verification.checks.map(check => (
                    <li key={check.scope}>
                      {t(check.scope === "catalog" ? "合约目录" : "历史数据接口")}：
                      {t(checkStates[check.state] ?? "验证失败")}
                    </li>
                  ))}
                </ul>
              )}
          </section>
        );
      })}
      <form
        onSubmit={async event => {
          event.preventDefault();
          if (!source || !schema) return;
          const ok = await run("research.connections.save", {
            id: editing?.id ?? `connection-${crypto.randomUUID()}`,
            name,
            source: source.id,
            revision: editing?.revision ?? "",
            requests_per_minute: Number(rate),
            remember,
            credential,
            credential_action: editing && !credential ? "keep" : "replace",
          });
          setCredential("");
          if (ok) edit(null);
        }}
      >
        <h3>{t(editing ? "编辑连接" : "新建连接")}</h3>
        <fieldset disabled={busy || !snapshot?.research?.online} className="futures-fields">
          <label>
            {t("数据源")}
            <select
              aria-label={t("数据源")}
              required
              value={sourceId}
              disabled={!!editing}
              onChange={event => {
                setSourceId(event.target.value);
                setCredential("");
                setRemember(false);
                setRate(
                  String(
                    sources.find(item => item.id === event.target.value)?.connection
                      ?.requests_per_minute_default ?? 60,
                  ),
                );
              }}
            >
              <option value="">{t("选择数据源")}</option>
              {sources.map(item => (
                <option key={item.id} value={item.id}>
                  {item.name}
                </option>
              ))}
            </select>
          </label>
          <label>
            {t("连接名称")}
            <input
              required
              maxLength={128}
              value={name}
              onChange={event => setName(event.target.value)}
            />
          </label>
          {schema && (
            <>
              <label>
                {locale === "zh-CN" ? schema.credential_label_zh : schema.credential_label_en}
                <input
                  type="password"
                  autoComplete="off"
                  maxLength={schema.credential_max_length}
                  required={schema.credential_required && !editing?.credential_ready}
                  value={credential}
                  onChange={event => setCredential(event.target.value)}
                />
              </label>
              {editing && <p>{t("凭据留空表示保留；已提交的任务不受连接修改影响。")}</p>}
              <label>
                {t("每分钟请求上限")}
                <input
                  type="number"
                  required
                  min={1}
                  max={schema.requests_per_minute_max}
                  value={rate}
                  onChange={event => setRate(event.target.value)}
                />
              </label>
              {schema.remember_allowed && (
                <label className="native-plugin-choice">
                  <input
                    type="checkbox"
                    checked={remember}
                    onChange={event => setRemember(event.target.checked)}
                  />
                  {t("在本机记住凭据")}
                </label>
              )}
            </>
          )}
        </fieldset>
        <button disabled={busy || !schema || !snapshot?.research?.online}>{t("保存连接")}</button>{" "}
        {editing && (
          <button type="button" disabled={busy} onClick={() => edit(null)}>
            {t("取消编辑")}
          </button>
        )}
      </form>
      <p>
        {t("验证成功表示接口接受请求，不保证所选时段有数据。删除连接不删除已提交任务或历史数据。")}
      </p>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </section>
  );
}
