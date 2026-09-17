import { useEffect, useState } from "react";
import { request, type Provider } from "../api/client";

export function DataSources({ token }: { token: string }) {
  const [providers, setProviders] = useState<Provider[]>([]);
  const [secret, setSecret] = useState("");
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState("");
  const [error, setError] = useState("");
  const refresh = () =>
    request<Provider[]>("/data/providers", token).then(setProviders);
  useEffect(() => {
    void refresh().catch((e) => setError(String(e)));
  }, [token]);
  async function action(id: string, kind: "save" | "clear" | "check") {
    setBusy(true);
    setError("");
    setMessage("");
    try {
      if (kind === "check") {
        const result = await request<{ message: string }>(
          `/data/providers/${id}/check`,
          token,
          {},
          100000,
        );
        setMessage(result.message);
      } else {
        await request(`/data/providers/${id}/credential`, token, {
          token: kind === "clear" ? "" : secret,
        });
        setSecret("");
        await refresh();
        setMessage(
          kind === "clear"
            ? "本机凭据已移除。"
            : "Token 已保存，可前往数据工作区同步。尚未验证接口权限。",
        );
      }
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  return (
    <>
      <h2 className="settings-section-title">已安装的数据源插件</h2>
      {providers.map((p) => (
        <section key={p.id}>
          <div className="setting-row">
            <div>
              <b>{p.name}</b>
              <small>插件 {p.version} · 接口版本 1</small>
            </div>
            <span>{p.configured ? "已配置凭据" : "未配置"}</span>
          </div>
          {p.credential_error && (
            <div className="alert" role="alert">
              {p.credential_error}
            </div>
          )}
          <p className="settings-note">
            支持：{p.capabilities.map((c) => c.label).join("、")}
            。同步操作位于「数据」工作区。
          </p>
          <form
            onSubmit={(e) => {
              e.preventDefault();
              void action(p.id, "save");
            }}
          >
            <div className="setting-row">
              <label htmlFor={`token-${p.id}`}>Tushare Token</label>
              <input
                id={`token-${p.id}`}
                type="password"
                autoComplete="off"
                maxLength={256}
                value={secret}
                placeholder={
                  p.configured
                    ? "输入新 Token 以替换"
                    : "粘贴 Tushare Pro Token"
                }
                onChange={(e) => setSecret(e.target.value)}
              />
            </div>
            <div className="source-actions">
              <button type="submit" disabled={busy || !secret.trim()}>
                保存 Token
              </button>
              <button
                type="button"
                disabled={busy || !p.configured}
                onClick={() => void action(p.id, "check")}
              >
                {busy ? "处理中…" : "测试已保存的凭据"}
              </button>
              <button
                type="button"
                disabled={busy || !p.configured}
                onClick={() => void action(p.id, "clear")}
              >
                移除凭据
              </button>
            </div>
          </form>
          <p className="settings-note">
            凭据加密保存在本机服务目录，由当前系统用户读写；不会回显或写入数据集。各接口的积分与调用权限由
            Tushare 账号决定。
          </p>
        </section>
      ))}
      {message && (
        <div className="notice" role="status">
          {message}
        </div>
      )}
      {error && (
        <div className="alert" role="alert">
          {error}
        </div>
      )}
    </>
  );
}
