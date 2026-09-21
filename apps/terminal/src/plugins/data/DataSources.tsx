import type { RequestClient } from "../../api/requests";
import { useEffect, useRef, useState } from "react";

import type { components } from "../../api/schema";
import { readProviders, type Provider } from "./client";

type Value = string | number | boolean | null;
type Configuration = components["schemas"]["ConfigurationState"];

function ProviderConfiguration({
  provider,
  api,
  onUpdated,
}: {
  provider: Provider;
  api: RequestClient;
  onUpdated: () => void;
}) {
  const fields = provider.configuration.fields;
  const [saved, setSaved] = useState<Configuration | null>(null);
  const [values, setValues] = useState<Record<string, Value>>({});
  const [secrets, setSecrets] = useState<Record<string, string | null>>({});
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState("");
  const [error, setError] = useState("");
  const [dirty, setDirty] = useState(false);
  const [verified, setVerified] = useState(false);
  const sequence = useRef(0);
  const form = useRef<HTMLFormElement>(null);
  const path = `/data/providers/${provider.id}/configuration`;

  function apply(configuration: Configuration) {
    setSaved(configuration);
    setValues(
      Object.fromEntries(
        fields
          .filter((field) => !field.secret)
          .map((field) => {
            const value = configuration.values[field.id];
            return [
              field.id,
              typeof value === "string" ||
              typeof value === "number" ||
              typeof value === "boolean"
                ? value
                : (field.default ?? null),
            ];
          }),
      ),
    );
    setSecrets({});
    setDirty(false);
    setVerified(false);
  }
  async function refresh() {
    const current = ++sequence.current;
    setBusy(true);
    setError("");
    setMessage("");
    try {
      const configuration = provider.configuration
        ? await api.request<Configuration>(path)
        : {
            provider: provider.id,
            revision: 0,
            schema_version: 1,
            values: {},
            secret_fields: [],
            configured: provider.configured,
          };
      if (current === sequence.current) apply(configuration);
    } catch (e) {
      if (current === sequence.current) setError(String(e));
    } finally {
      if (current === sequence.current) setBusy(false);
    }
  }
  useEffect(() => {
    void refresh();
    return () => {
      sequence.current++;
    };
  }, [api, provider.id]);

  function changed() {
    setDirty(true);
    setVerified(false);
    setMessage("");
    setError("");
  }
  function payload() {
    return {
      expected_revision: saved!.revision,
      values: Object.fromEntries(
        fields
          .filter((field) => !field.secret)
          .map((field) => {
            const value = values[field.id] ?? null;
            return [
              field.id,
              value === "" && !field.required
                ? null
                : field.type === "integer" && value !== null && value !== ""
                  ? Number(value)
                  : value,
            ];
          }),
      ),
      secrets,
    };
  }
  const ready = fields.every((field) => {
    if (!field.required) return true;
    if (field.secret)
      return secrets[field.id] === null
        ? false
        : !!secrets[field.id] || !!saved?.secret_fields.includes(field.id);
    return values[field.id] !== "" && values[field.id] != null;
  });
  async function action(kind: "save" | "check") {
    if (!saved || !form.current?.reportValidity()) return;
    const current = ++sequence.current;
    setBusy(true);
    setError("");
    setMessage("");
    try {
      if (kind === "check") {
        const result = await api.request<{ message: string }>(
          `${path}/check`,
          payload(),
          100000,
        );
        if (current !== sequence.current) return;
        setVerified(true);
        setMessage(
          `${result.message}。${dirty ? "草稿尚未应用，保存后才影响后续任务。" : "已保存配置通过测试。"}`,
        );
      } else {
        const configuration = await api.request<Configuration>(path, payload());
        if (current !== sequence.current) return;
        apply(configuration);
        onUpdated();
        setMessage(
          `配置已保存，仅影响后续任务。${verified ? "当前草稿已通过测试。" : "尚未验证接口可用性。"}`,
        );
      }
    } catch (e) {
      if (current === sequence.current) setError(String(e));
    } finally {
      if (current === sequence.current) setBusy(false);
    }
  }
  const configurationError = saved ? saved.error : provider.credential_error;
  return (
    <section className="provider-configuration" aria-label={provider.name}>
      <div className="setting-row">
        <div>
          <b>{provider.name}</b>
          <small>
            插件 {provider.version} · 接口版本 {provider.api_version}
          </small>
        </div>
        <span>
          {(saved?.configured ?? provider.configured) ? "配置就绪" : "待配置"}
        </span>
      </div>
      <p className="settings-note">
        {provider.demo && <strong>合成示例 · 非真实行情。 </strong>}
        {provider.description}
        支持：
        {provider.capabilities.map((capability) => capability.label).join("、")}
        。 同步操作位于「数据」工作区。
      </p>
      {configurationError && (
        <div className="alert" role="alert">
          {configurationError}
        </div>
      )}
      <form
        ref={form}
        onSubmit={(event) => {
          event.preventDefault();
          void action("save");
        }}
      >
        <fieldset disabled={busy || !saved}>
          {fields.map((field) => {
            const stored = saved?.secret_fields.includes(field.id);
            const removing = field.secret && secrets[field.id] === null;
            const value = field.secret
              ? (secrets[field.id] ?? "")
              : (values[field.id] ?? "");
            const inputId = `provider-${provider.id}-${field.id}`;
            return (
              <div className="setting-row" key={field.id}>
                <div>
                  <label htmlFor={inputId}>
                    {field.label}
                    {field.required ? " *" : ""}
                  </label>
                  <small id={`${inputId}-description`}>
                    {field.description}
                    {field.secret &&
                      (removing
                        ? " 保存时移除已存凭据。"
                        : stored
                          ? " 已保存；留空保持不变。"
                          : " 尚未保存。")}
                  </small>
                </div>
                <div className="provider-field-control">
                  <input
                    id={inputId}
                    aria-label={field.label}
                    aria-describedby={`${inputId}-description`}
                    type={
                      field.secret
                        ? "password"
                        : field.type === "integer"
                          ? "number"
                          : field.type === "boolean"
                            ? "checkbox"
                            : "text"
                    }
                    autoComplete="off"
                    required={
                      field.required &&
                      field.type !== "boolean" &&
                      (!field.secret || (!stored && !removing))
                    }
                    minLength={field.min_length ?? undefined}
                    maxLength={field.max_length ?? undefined}
                    min={field.minimum ?? undefined}
                    max={field.maximum ?? undefined}
                    step={field.type === "integer" ? 1 : undefined}
                    placeholder={field.placeholder}
                    value={
                      field.type === "boolean" && !field.secret
                        ? undefined
                        : String(value)
                    }
                    checked={
                      field.type === "boolean" && !field.secret
                        ? value === true
                        : undefined
                    }
                    onChange={(event) => {
                      changed();
                      if (field.secret) {
                        const next = event.target.value;
                        setSecrets((current) => {
                          const updated = { ...current };
                          if (next) updated[field.id] = next;
                          else delete updated[field.id];
                          return updated;
                        });
                      } else
                        setValues((current) => ({
                          ...current,
                          [field.id]:
                            field.type === "boolean"
                              ? event.target.checked
                              : event.target.value,
                        }));
                    }}
                  />
                  {field.secret && stored && (
                    <button
                      type="button"
                      onClick={() => {
                        changed();
                        setSecrets((current) => {
                          const updated = { ...current };
                          if (removing) delete updated[field.id];
                          else updated[field.id] = null;
                          return updated;
                        });
                      }}
                    >
                      {removing ? "保留已存凭据" : "移除已存凭据"}
                    </button>
                  )}
                </div>
              </div>
            );
          })}
          {!fields.length && (
            <p className="settings-note">此数据源无需填写配置。</p>
          )}
          <div className="source-actions">
            <button type="submit" disabled={!dirty}>
              保存配置
            </button>
            <button
              type="button"
              disabled={!ready}
              onClick={() => void action("check")}
            >
              {busy ? "处理中…" : "测试当前配置"}
            </button>
          </div>
        </fieldset>
      </form>
      <button type="button" disabled={busy} onClick={() => void refresh()}>
        {dirty ? "刷新已保存配置（覆盖草稿）" : "刷新已保存配置"}
      </button>
      {fields.some((field) => field.secret) && (
        <p className="settings-note">
          凭据加密保存在本机，不会回显或写入数据集。测试草稿不会保存配置。
        </p>
      )}
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
    </section>
  );
}

function ConnectionManagement({
  provider,
  api,
  onUpdated,
}: {
  provider: Provider;
  api: RequestClient;
  onUpdated: () => void;
}) {
  const [name, setName] = useState(provider.name);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  useEffect(() => setName(provider.name), [provider.name]);
  const lifecycle = provider.lifecycle;
  const state = lifecycle.state;
  const verification = provider.verification;
  async function update(next: "enabled" | "disabled" | "archived") {
    setBusy(true);
    setError("");
    try {
      await api.request(`/data/connections/${provider.id}`, {
        expected_revision: lifecycle.revision,
        name,
        state: next,
      });
      onUpdated();
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  async function verify() {
    setBusy(true);
    setError("");
    try {
      await api.request(`/data/providers/${provider.id}/verify`, {}, 100000);
      onUpdated();
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  return (
    <section
      className="connection-management"
      aria-label={`${provider.name}连接管理`}
    >
      <div className="source-actions">
        <input
          aria-label={`${provider.name}名称`}
          value={name}
          disabled={busy}
          onChange={(e) => setName(e.target.value)}
        />
        <button
          disabled={busy || !name.trim() || name === provider.name}
          onClick={() => void update(state)}
        >
          保存名称
        </button>
        <span>
          {state === "enabled"
            ? "可用于新任务"
            : state === "disabled"
              ? "已停用"
              : "已归档"}
        </span>
        {state === "enabled" ? (
          <button disabled={busy} onClick={() => void update("disabled")}>
            停用
          </button>
        ) : (
          <button disabled={busy} onClick={() => void update("enabled")}>
            恢复连接
          </button>
        )}
        {state !== "archived" && (
          <button disabled={busy} onClick={() => void update("archived")}>
            归档
          </button>
        )}
        <button
          disabled={busy || state !== "enabled" || !provider.configured}
          onClick={() => void verify()}
        >
          验证已保存配置
        </button>
      </div>
      <p className="settings-note">
        {
          (
            {
              never: "尚未验证",
              verified: "验证成功",
              failed: "验证失败",
              stale: "验证已过期",
            } as const
          )[verification.status]
        }
        {verification?.checked_at
          ? ` · ${new Date(verification.checked_at * 1000).toLocaleString()}`
          : ""}{" "}
        · {verification.message}
      </p>
      <p className="settings-note">
        停用或归档只阻止新同步，既有任务和历史数据保留；需要停止已提交任务时，请在任务中心取消。
      </p>
      {error && (
        <div className="alert" role="alert">
          {error}
        </div>
      )}
    </section>
  );
}

export function DataSources({ api }: { api: RequestClient }) {
  const [providers, setProviders] = useState<Provider[]>([]);
  const [error, setError] = useState("");
  const [showArchived, setShowArchived] = useState(false);
  async function refreshProviders() {
    try {
      setProviders(await readProviders(api));
    } catch (e) {
      setError(String(e));
    }
  }
  const [plugin, setPlugin] = useState("");
  const [connectionName, setConnectionName] = useState("");
  const [creating, setCreating] = useState(false);
  async function addConnection() {
    setCreating(true);
    setError("");
    try {
      await api.request("/data/connections", {
        provider: plugin,
        name: connectionName,
      });
      setProviders(await readProviders(api));
      setConnectionName("");
    } catch (e) {
      setError(String(e));
    } finally {
      setCreating(false);
    }
  }
  useEffect(() => {
    let active = true;
    setError("");
    void readProviders(api)
      .then((result) => {
        if (active) setProviders(result);
      })
      .catch((e) => {
        if (active) setError(String(e));
      });
    return () => {
      active = false;
    };
  }, [api]);
  return (
    <>
      <h2 className="settings-section-title">数据源与连接</h2>
      <p className="settings-note">
        每个连接独立管理配置、凭据和数据版本，可分别启用、停用或归档。
      </p>
      <div className="source-actions">
        <select
          aria-label="连接插件"
          value={plugin}
          onChange={(e) => setPlugin(e.target.value)}
          disabled={creating}
        >
          <option value="">选择供应商插件</option>
          {providers
            .filter((p) => !p.connection_id)
            .map((p) => (
              <option key={p.id} value={p.id}>
                {p.name}
              </option>
            ))}
        </select>
        <input
          aria-label="连接名称"
          placeholder="例如：研究账户"
          value={connectionName}
          onChange={(e) => setConnectionName(e.target.value)}
          disabled={creating}
        />
        <button
          disabled={creating || !plugin || !connectionName.trim()}
          onClick={() => void addConnection()}
        >
          新增连接
        </button>
      </div>
      <div className="source-actions">
        <label>
          <input
            type="checkbox"
            checked={showArchived}
            onChange={(e) => setShowArchived(e.target.checked)}
          />
          显示归档连接
        </label>
        <button onClick={() => void refreshProviders()}>刷新连接列表</button>
      </div>
      {providers
        .filter((p) => showArchived || p.lifecycle.state !== "archived")
        .map((provider) => (
          <div key={provider.id}>
            <ProviderConfiguration
              provider={provider}
              api={api}
              onUpdated={() => void refreshProviders()}
            />
            <ConnectionManagement
              provider={provider}
              api={api}
              onUpdated={() => void refreshProviders()}
            />
          </div>
        ))}
      {error && (
        <div className="alert" role="alert">
          {error}
        </div>
      )}
    </>
  );
}
