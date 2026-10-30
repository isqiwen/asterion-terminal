import type { CommandHandler, CommandCall } from "../bridge/commands";
import { request } from "../bridge/client";
import { desktop, open } from "../bridge/desktop";
import { useEffect, useState } from "react";
import type { NativePluginInfo, Snapshot } from "../bridge/client";
import { translate } from "../i18n";
import {
  availableDataTaskPlugins,
  defaultDataTaskPlugins,
  requiredPluginIds,
  selectPlugin,
} from "../host/native-plugins";
import { ErrorNotice, BackendError, asDisplayError, type DisplayError } from "../i18n/errors";
import { SettingsSection } from "./Section";
import { kindLabels } from "./service-state";
const t = (key: string) => translate("host", key);
type Props = {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: CommandHandler;
  // Services are started and stopped on their own page.
  openServices: () => void;
};
type Service = NonNullable<Snapshot["nodes"][number]["health"]>["services"][number];
// What a capability gives the user, in words; anything else keeps its identifier.
const capabilityNames: Record<string, string> = {
  "asterion.history": "分钟 K 线",
  "asterion.history.tables": "全市场数据表",
  "asterion.history.connection": "供应商账号",
  "asterion.history.response": "原始响应记录",
};
const offers = (item: NativePluginInfo) =>
  item.capabilities
    .map(c => (capabilityNames[c.id] ? t(capabilityNames[c.id]) : c.id))
    .join("、") || "—";
// One plugin a service may run. A required plugin with a single version is
// always on and shows as text; otherwise choosing a version replaces the other.
function PluginChoice({
  item,
  fixed,
  required,
  checked,
  disabled,
  onChange,
}: {
  item: NativePluginInfo;
  fixed: boolean;
  required: boolean;
  checked: boolean;
  disabled: boolean;
  onChange: (checked: boolean) => void;
}) {
  if (fixed)
    return (
      <div className="settings-check native-plugin-choice">
        <span aria-hidden="true">✓</span>
        <span>
          {item.id} · {item.version} · {t("随应用启用")}
        </span>
      </div>
    );
  return (
    <label className="settings-check native-plugin-choice">
      <input
        type="checkbox"
        checked={checked}
        disabled={disabled || (required && checked)}
        onChange={event => onChange(event.target.checked)}
      />
      <span>
        {item.id} · {item.version}
      </span>
    </label>
  );
}
function ServicePlugins({
  service,
  items,
  busy,
  trade,
  openServices,
}: {
  service: Service;
  items: NativePluginInfo[];
  busy: boolean;
  trade: Props["trade"];
  openServices: () => void;
}) {
  const [draft, setDraft] = useState<{ revision: string; hashes: string[] } | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [saved, setSaved] = useState(false);
  const selected = draft?.hashes ?? service.plugin_artifacts;
  const stale = draft !== null && draft.revision !== service.revision;
  const stopped = service.state === "stopped" && !service.desired_running;
  const editable = stopped && !busy && !stale;
  const options = availableDataTaskPlugins(items);
  const required = requiredPluginIds(items);
  const versions = (id: string) => options.filter(item => item.id === id).length;
  const pinned = service.plugin_artifacts.filter(
    hash => !options.some(item => item.sha256 === hash),
  );
  const toggle = (hash: string, checked: boolean) => {
    setSaved(false);
    setDraft({
      revision: draft?.revision ?? service.revision,
      hashes: selectPlugin(selected, hash, checked, items),
    });
  };
  const run = async (...args: CommandCall) => {
    setError("");
    try {
      await trade(...args);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  };
  return (
    <section className="settings-item" aria-label={`${t("服务插件")} ${service.id}`}>
      <div className="settings-item-row">
        <div>
          <h3>{t(kindLabels[service.kind])}</h3>
          <p>
            {t(
              stopped
                ? "已停止，可以修改插件；启动后生效。"
                : "运行中。修改插件前先在本机服务页停止它及其下载任务。",
            )}
          </p>
        </div>
        <div className="settings-actions">
          {stopped ? (
            <>
              <button
                disabled={busy || draft === null}
                onClick={() => {
                  setDraft(null);
                  setError("");
                  setSaved(false);
                }}
              >
                {t("撤销修改")}
              </button>
              <button
                className="primary"
                disabled={!editable || draft === null}
                onClick={async () => {
                  if (
                    await run("node.plugins.configure", {
                      service: service.id,
                      revision: draft?.revision ?? service.revision,
                      plugins: selected,
                    })
                  ) {
                    setDraft(null);
                    setSaved(true);
                  }
                }}
              >
                {t("保存插件配置")}
              </button>
            </>
          ) : (
            <button onClick={openServices}>{t("前往本机服务")}</button>
          )}
        </div>
      </div>
      {stale && <p role="alert">{t("服务配置已变化，请撤销修改后重新选择。")}</p>}
      <div className="settings-list" aria-label={t("启用的数据与研究插件")}>
        {options.map(item => (
          <PluginChoice
            key={item.sha256}
            item={item}
            fixed={required.has(item.id) && versions(item.id) === 1}
            required={required.has(item.id)}
            checked={selected.includes(item.sha256)}
            disabled={!editable}
            onChange={checked => toggle(item.sha256, checked)}
          />
        ))}
        {pinned.map(hash => (
          <label className="settings-check native-plugin-choice" key={hash}>
            <input
              type="checkbox"
              disabled={!editable}
              checked={selected.includes(hash)}
              onChange={event => toggle(hash, event.target.checked)}
            />
            <span>
              {t("已部署版本（本机目录中不存在）")} · <code>{hash.slice(0, 12)}</code>
            </span>
          </label>
        ))}
      </div>
      {saved && <p role="status">{t("插件配置已保存，启动服务后生效。")}</p>}
      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </section>
  );
}
export function NativePlugins({ snapshot, busy, trade, openServices }: Props) {
  const [error, setError] = useState<DisplayError>("");
  const [candidate, setCandidate] = useState<{ path: string; info: NativePluginInfo } | null>(null);
  const [trusted, setTrusted] = useState(false);
  const [installing, setInstalling] = useState(false);
  const [removing, setRemoving] = useState("");
  // Until the user changes it, a new service starts with the bundled plugins.
  const [chosenPlugins, setInitialPlugins] = useState<string[] | null>(null);
  const inspect = async () => {
    setError("");
    setInitialPlugins([]);
    try {
      await trade("native.plugins.inspect");
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  };
  useEffect(() => {
    void inspect(); /* Read inventory only when opening the page. */
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  const inventory = snapshot?.native_plugins;
  const initialPlugins = chosenPlugins ?? defaultDataTaskPlugins(inventory?.items ?? []);
  const node = snapshot?.nodes.find(item => item.id === "local");
  const services =
    node?.health?.services.filter(
      service => service.kind === "research" || service.kind === "data",
    ) ?? [];
  const required = requiredPluginIds(inventory?.items ?? []);
  return (
    <div className="native-plugin-manager" role="region" aria-label={t("原生插件管理")}>
      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {inventory && (
        <SettingsSection
          title={t("服务启用的插件")}
          description={t(
            "数据源插件由历史数据服务和研究服务加载，每个服务保存自己的清单。风控编译在服务里，不需要安装。",
          )}
        >
          <div className="settings-list">
            {services.map(service => (
              <ServicePlugins
                key={service.id}
                service={service}
                items={inventory.items}
                busy={busy}
                trade={trade}
                openServices={openServices}
              />
            ))}
          </div>
          {services.length === 0 && (
            <fieldset disabled={busy} className="settings-list">
              <legend className="settings-hint">
                {t("还没有本机数据与研究服务。应用自带的插件始终启用；自行安装的可以选择。")}
              </legend>
              {availableDataTaskPlugins(inventory.items).map(item => (
                <PluginChoice
                  key={item.sha256}
                  item={item}
                  fixed={
                    required.has(item.id) &&
                    inventory.items.filter(other => other.id === item.id && other.selectable)
                      .length === 1
                  }
                  required={required.has(item.id)}
                  checked={initialPlugins.includes(item.sha256)}
                  disabled={busy}
                  onChange={checked =>
                    setInitialPlugins(previous =>
                      selectPlugin(
                        previous ?? initialPlugins,
                        item.sha256,
                        checked,
                        inventory.items,
                      ),
                    )
                  }
                />
              ))}
              <div className="settings-actions">
                <button
                  className="primary"
                  onClick={async () => {
                    setError("");
                    try {
                      await trade("node.research.local.create", { plugins: initialPlugins });
                      setInitialPlugins([]);
                    } catch (reason) {
                      setError(asDisplayError(reason));
                    }
                  }}
                >
                  {t("创建数据与研究服务")}
                </button>
              </div>
            </fieldset>
          )}
        </SettingsSection>
      )}
      <SettingsSection
        title={t("插件目录")}
        description={t("可供服务选用的插件：应用自带的，以及你安装的。")}
        actions={
          <button disabled={busy} onClick={() => void inspect()}>
            {t("刷新插件目录")}
          </button>
        }
      >
        {inventory && (
          <>
            {inventory.items.length === 0 && (
              <p className="settings-hint">{t("插件目录中没有动态库。")}</p>
            )}
            <table className="data-table native-plugin-catalog" aria-label={t("原生插件目录")}>
              <thead>
                <tr>
                  <th>{t("插件")}</th>
                  <th>{t("版本")}</th>
                  <th>{t("提供")}</th>
                  <th>{t("状态")}</th>
                  <th>{t("来源")}</th>
                </tr>
              </thead>
              <tbody>
                {inventory.items.map(item => (
                  <tr key={`${item.managed}:${item.file}`}>
                    <td>
                      <span>{item.id || item.file}</span>
                      <details>
                        <summary>{t("文件与摘要")}</summary>
                        <small>
                          {item.file}
                          <code title={item.sha256}>{item.sha256}</code>
                        </small>
                      </details>
                    </td>
                    <td>{item.version || "—"}</td>
                    <td>{offers(item)}</td>
                    <td>
                      {item.state === "available" ? (
                        t("可用")
                      ) : (
                        <ErrorNotice error={new BackendError("unavailable", item.error)} />
                      )}
                    </td>
                    <td>
                      {item.managed ? (
                        removing === item.file ? (
                          <>
                            <p>{t("仅移除可部署副本；已部署服务、任务和历史数据保留。")}</p>
                            <button
                              className="danger"
                              disabled={busy || installing}
                              onClick={async () => {
                                setError("");
                                try {
                                  await trade("native.plugins.uninstall", {
                                    file: item.file,
                                    sha256: item.sha256,
                                  });
                                  setRemoving("");
                                } catch (reason) {
                                  setError(asDisplayError(reason));
                                }
                              }}
                            >
                              {t("确认卸载插件")}
                            </button>
                            <button onClick={() => setRemoving("")}>{t("取消")}</button>
                          </>
                        ) : (
                          <>
                            {t("自行安装")}
                            <br />
                            <button
                              disabled={busy || installing || !item.sha256}
                              onClick={() => setRemoving(item.file)}
                            >
                              {t("卸载插件")}
                            </button>
                          </>
                        )
                      ) : (
                        t("应用自带")
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
            <details>
              <summary>{t("目录位置")}</summary>
              <dl>
                <dt>{t("应用插件目录")}</dt>
                <dd>
                  <code>{inventory.directory}</code>
                </dd>
                <dt>{t("用户插件目录")}</dt>
                <dd>
                  <code>{inventory.managed_directory}</code>
                </dd>
              </dl>
            </details>
          </>
        )}
      </SettingsSection>
      <SettingsSection
        title={t("安装原生插件")}
        description={t(
          "插件是本机代码，读取它的信息就会运行它。安装后在上面为服务选择并保存；每个服务只能启用同一插件的一个版本。",
        )}
      >
        <label className="settings-check">
          <input
            type="checkbox"
            checked={trusted}
            disabled={installing}
            onChange={event => {
              setTrusted(event.target.checked);
              setCandidate(null);
            }}
          />
          {t("我信任此动态库的来源；读取插件信息会执行其本机代码。")}
        </label>
        {candidate && (
          <dl className="settings-card">
            <dt>{t("插件")}</dt>
            <dd>
              {candidate.info.id} · {candidate.info.version}
            </dd>
            <dt>{t("提供")}</dt>
            <dd>{offers(candidate.info)}</dd>
            <dt>SHA-256</dt>
            <dd>
              <code>{candidate.info.sha256}</code>
            </dd>
          </dl>
        )}
        <div className="settings-actions">
          <button
            disabled={busy || installing || !trusted}
            onClick={async () => {
              setInstalling(true);
              setError("");
              setCandidate(null);
              try {
                const path = await open({
                  title: t("选择插件动态库"),
                  filters: [
                    {
                      name: t("原生插件"),
                      extensions: [desktop().platform === "darwin" ? "dylib" : "so"],
                    },
                  ],
                });
                if (typeof path === "string") {
                  const preview = await request("native.plugins.preview", { path });
                  if (preview.plugin_candidate)
                    setCandidate({ path, info: preview.plugin_candidate });
                }
              } catch (reason) {
                setError(asDisplayError(reason));
              } finally {
                setInstalling(false);
              }
            }}
          >
            {t("选择插件动态库")}
          </button>
          {candidate && (
            <button
              className="primary"
              disabled={busy || installing}
              onClick={async () => {
                setInstalling(true);
                setError("");
                try {
                  await trade("native.plugins.install", {
                    path: candidate.path,
                    sha256: candidate.info.sha256,
                  });
                  setCandidate(null);
                  setTrusted(false);
                } catch (reason) {
                  setError(asDisplayError(reason));
                } finally {
                  setInstalling(false);
                }
              }}
            >
              {t("确认安装插件")}
            </button>
          )}
        </div>
      </SettingsSection>
    </div>
  );
}
