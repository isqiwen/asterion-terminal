import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { translate, type MessageValues, getLocale } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { NodeServices } from "./NodeServices";
import { useState } from "react";
import type { TerminalCommand, Snapshot } from "../bridge/client";
type Profile = {
  name: string;
  host: string;
  port: string;
  session: string;
  mode: "paper";
  ca_file: string;
  certificate_file: string;
  private_key_file: string;
};
const key = "asterion.service-connections.v1";
const blank: Profile = {
  name: "",
  host: "",
  port: "",
  session: "",
  mode: "paper",
  ca_file: "",
  certificate_file: "",
  private_key_file: "",
};
function load(): {
  profiles: Profile[];
  error: DisplayError;
} {
  try {
    const raw = localStorage.getItem(key);
    if (!raw) return { profiles: [], error: "" };
    const value: unknown = JSON.parse(raw);
    if (
      !Array.isArray(value) ||
      !value.every(
        p =>
          p &&
          Object.keys(p).length === Object.keys(blank).length &&
          Object.keys(blank).every(k => typeof p[k] === "string") &&
          p.mode === "paper",
      ) ||
      new Set(value.map(p => p.name)).size !== value.length
    )
      throw new Error("invalid profiles");
    return { profiles: value, error: "" };
  } catch {
    return {
      profiles: [],
      error: t("连接配置无法读取。原始内容未改写，请检查本机配置后重新打开设置。"),
    };
  }
}
export function Connections({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
}) {
  const [initial] = useState(load);
  const [profiles, setProfiles] = useState(initial.profiles);
  const [profile, setProfile] = useState<Profile>({ ...blank });
  const [selected, setSelected] = useState("");
  const [error, setError] = useState<DisplayError>(initial.error);
  const connection = snapshot?.connection;
  async function run(method: TerminalCommand, params: Record<string, unknown> = {}) {
    setError("");
    try {
      await trade(method, params);
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  }
  function persist(next: Profile[]) {
    try {
      localStorage.setItem(key, JSON.stringify(next));
      setProfiles(next);
      setError("");
      return true;
    } catch {
      setError(t("保存失败，请检查本机存储权限。"));
      return false;
    }
  }
  return (
    <section className="service-connections">
      <NodeServices snapshot={snapshot} busy={busy} trade={trade} />
      <details className="direct-connection">
        <summary>{t("高级：直接连接交易服务")}</summary>
        <h2>{t("交易服务 · 模拟")}</h2>
        {connection?.health && (
          <p>
            {t("业务状态：")}
            {connection.state !== "connected"
              ? t("失联 · 最后确认状态")
              : ({
                  ready: t("就绪"),
                  awaiting_input: t("等待初始化"),
                  degraded: t("异常 · 需要恢复"),
                }[connection.health.phase] ?? connection.health.phase)}
            {t("· 最近心跳")}
            {new Date(connection.last_heartbeat_ms).toLocaleTimeString(getLocale(), {
              hour12: false,
            })}
            {t("· 延迟")}
            {connection.latency_ms} ms ·{" "}
            {connection.transport === "local"
              ? t("重启 {p0} 次", { p0: connection.restarts })
              : t("重连 {p0} 次", { p0: connection.reconnects })}
          </p>
        )}
        <p role="status">
          {connection
            ? `${connection.state === "connected" ? t("已连接") : t("已断开")} · ${connection.transport === "tcp_tls" ? `${connection.host}:${connection.port} · ${connection.session}` : t("本机交易进程")}`
            : t("尚未连接交易服务")}
        </p>
        <p>{t("在交易工作区创建或恢复模拟会话。")}</p>
        {error && (
          <p className="alert" role="alert">
            <ErrorNotice error={error} namespace="host" />
          </p>
        )}
        {connection && (
          <div className="source-actions">
            <button disabled={busy} onClick={() => void run("paper.close")}>
              {t("断开连接")}
            </button>
            {connection.transport === "tcp_tls" && (
              <button disabled={busy} onClick={() => void run("paper.reconnect")}>
                {t("重新连接")}
              </button>
            )}
          </div>
        )}
        <details>
          <summary>{t("直接连接已部署交易服务")}</summary>
          <form
            onSubmit={event => {
              event.preventDefault();
              if (profiles.some(p => p.name === profile.name && p.name !== selected)) {
                setError(t("配置名称已存在"));
                return;
              }
              const next = [...profiles.filter(p => p.name !== selected), profile];
              if (persist(next)) setSelected(profile.name);
            }}
          >
            <fieldset disabled={busy || !!initial.error}>
              <label className="terminal-setting">
                {t("已保存配置")}
                <select
                  aria-label={t("已保存配置")}
                  value={selected}
                  onChange={e => {
                    setSelected(e.target.value);
                    setProfile({ ...(profiles.find(p => p.name === e.target.value) ?? blank) });
                  }}
                >
                  <option value="">{t("新建连接")}</option>
                  {profiles.map(p => (
                    <option key={p.name} value={p.name}>
                      {p.name}
                    </option>
                  ))}
                </select>
              </label>
              <div className="futures-fields">
                {(
                  [
                    ["name", t("配置名称"), t("例如：研究机模拟账户")],
                    ["host", t("服务器地址"), t("DNS 名称或 IP，必须匹配服务端证书")],
                    ["port", t("端口"), "1–65535"],
                    ["session", t("会话标识"), t("与交易服务的 --session 一致")],
                  ] as const
                ).map(([field, label, placeholder]) => (
                  <label key={field}>
                    {label}
                    <input
                      aria-label={label}
                      required
                      value={profile[field]}
                      placeholder={placeholder}
                      {...(field === "port" ? { type: "number", min: 1, max: 65535 } : {})}
                      onChange={e => setProfile({ ...profile, [field]: e.target.value })}
                    />
                  </label>
                ))}
              </div>
              <details open>
                <summary>{t("连接身份 · 双向 TLS")}</summary>
                <p>
                  {t(
                    "填写 Terminal 所在机器上的 PEM 文件绝对路径。配置只保存路径，私钥由本机 C++ 通信层读取，不在界面显示；证书必须由服务端信任的专用 CA 签发。",
                  )}
                </p>
                <div className="futures-fields">
                  {(
                    [
                      ["ca_file", t("服务端 CA 文件")],
                      ["certificate_file", t("客户端证书文件")],
                      ["private_key_file", t("客户端私钥文件")],
                    ] as const
                  ).map(([field, label]) => (
                    <label key={field}>
                      {label}
                      <input
                        aria-label={label}
                        required
                        value={profile[field]}
                        onChange={e => setProfile({ ...profile, [field]: e.target.value })}
                      />
                    </label>
                  ))}
                </div>
              </details>
              <div className="source-actions">
                <button type="submit">{t("保存配置")}</button>
                <button
                  type="button"
                  disabled={!selected || !!connection}
                  onClick={() => {
                    const saved = profiles.find(p => p.name === selected);
                    if (saved) {
                      const { name: _name, ...params } = saved;
                      void run("paper.connect", params);
                    }
                  }}
                >
                  {t("连接已保存配置")}
                </button>
                <button
                  type="button"
                  disabled={!selected}
                  onClick={() => {
                    if (persist(profiles.filter(p => p.name !== selected))) {
                      setSelected("");
                      setProfile({ ...blank });
                    }
                  }}
                >
                  {t("删除配置")}
                </button>
              </div>
            </fieldset>
          </form>
        </details>
        <p>
          {t(
            "断开连接不会停止本机或远程服务。断线后自动重连最多 3 次，仍失败可手动重连；交易命令不会自动重发。配置按本机终端保存，不会自动连接。",
          )}
        </p>
      </details>
    </section>
  );
}
