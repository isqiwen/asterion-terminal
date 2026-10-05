import { request } from "../bridge/client";
import { open } from "../bridge/desktop";
import { useEffect, useState } from "react";
import type { NativePluginInfo, Snapshot, TerminalCommand } from "../bridge/client";
import { translate } from "../i18n";
import {
  availableDataTaskPlugins,
  defaultDataTaskPlugins,
  requiredPluginIds,
  selectPlugin,
} from "../host/native-plugins";
import { ErrorNotice, BackendError, asDisplayError, type DisplayError } from "../i18n/errors";
const t = (key: string) => translate("host", key);
type Props = {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
};
type Service = NonNullable<Snapshot["nodes"][number]["health"]>["services"][number];
// A selected required plugin cannot be cleared; choosing another version replaces it.
function PluginChoice({
  item,
  required,
  checked,
  onChange,
}: {
  item: NativePluginInfo;
  required: boolean;
  checked: boolean;
  onChange: (checked: boolean) => void;
}) {
  return (
    <label className="native-plugin-choice">
      <input
        type="checkbox"
        checked={checked}
        disabled={required && checked}
        onChange={event => onChange(event.target.checked)}
      />
      <span>
        {item.id} · {item.version}
        {required && ` · ${t("必需")}`}
      </span>
    </label>
  );
}
function ServicePlugins({
  service,
  items,
  busy,
  trade,
  maintenance,
  online,
}: {
  service: Service;
  items: NativePluginInfo[];
  busy: boolean;
  trade: Props["trade"];
  maintenance: boolean;
  online: boolean;
}) {
  const [draft, setDraft] = useState<{ revision: string; hashes: string[] } | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [saved, setSaved] = useState(false);
  const selected = draft?.hashes ?? service.plugin_artifacts;
  const stale = draft !== null && draft.revision !== service.revision;
  const stopped =
    service.state === "stopped" && !service.desired_running && service.active_workers === 0;
  const editable = stopped && online && !maintenance && !busy && !stale;
  const options = availableDataTaskPlugins(items);
  const required = requiredPluginIds(items);
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
  const run = async (method: TerminalCommand, params: Record<string, unknown>) => {
    setError("");
    try {
      await trade(method, params);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  };
  return (
    <section className="native-plugin-service" aria-label={`${t("服务插件")} ${service.id}`}>
      <h3>{service.id}</h3>
      <p>
        {stopped
          ? t("服务已停止，可以修改插件清单。")
          : t("先停止服务及下载任务，再修改插件清单。")}
      </p>
      <button
        disabled={busy || maintenance || !online || !service.desired_running}
        onClick={() =>
          void run("node.action", { id: "local", service: service.id, action: "stop" })
        }
      >
        {t("停止")}
      </button>{" "}
      <button
        disabled={busy || maintenance || !online || !stopped || draft !== null}
        onClick={() =>
          void run("node.action", { id: "local", service: service.id, action: "start" })
        }
      >
        {t("启动")}
      </button>
      {stale && <p role="alert">{t("服务配置已变化，请重新载入后选择插件。")}</p>}
      <fieldset disabled={!editable}>
        <legend>{t("启用的数据与任务插件")}</legend>
        {options.map(item => (
          <PluginChoice
            key={item.sha256}
            item={item}
            required={required.has(item.id)}
            checked={selected.includes(item.sha256)}
            onChange={checked => toggle(item.sha256, checked)}
          />
        ))}
        {pinned.map(hash => (
          <label className="native-plugin-choice" key={hash}>
            <input
              type="checkbox"
              checked={selected.includes(hash)}
              onChange={event => toggle(hash, event.target.checked)}
            />
            <span>
              {t("已部署版本（本机目录中不存在）")} · {hash.slice(0, 12)}
            </span>
          </label>
        ))}
      </fieldset>
      <button
        disabled={!editable || draft === null}
        onClick={async () => {
          if (
            await run("node.plugins.configure", {
              id: "local",
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
      </button>{" "}
      <button
        disabled={busy || draft === null}
        onClick={() => {
          setDraft(null);
          setError("");
          setSaved(false);
        }}
      >
        {t("重新载入配置")}
      </button>
      {saved && <p role="status">{t("插件配置已保存，启动服务后生效。")}</p>}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </section>
  );
}
export function NativePlugins({ snapshot, busy, trade }: Props) {
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
      await trade("node.local");
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
    node?.health?.services.filter(service => service.kind === "task" || service.kind === "data") ??
    [];
  return (
    <section className="native-plugin-manager" aria-label={t("原生插件管理")}>
      <h2>{t("原生插件管理")}</h2>
      <p>{t("插件目录表示可部署的版本；每个服务使用自己保存的启用清单。")}</p>
      <button disabled={busy} onClick={() => void inspect()}>
        {t("刷新插件目录")}
      </button>
      <section aria-label={t("安装原生插件")}>
        <h3>{t("安装原生插件")}</h3>
        <label className="native-plugin-choice">
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
        <button
          disabled={busy || installing || !trusted}
          onClick={async () => {
            setInstalling(true);
            setError("");
            setCandidate(null);
            try {
              const path = await open({
                title: t("选择插件动态库"),
                filters: [{ name: t("原生插件"), extensions: ["dylib"] }],
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
          <div>
            <p>
              {candidate.info.id} · {candidate.info.version}
            </p>
            <code>{candidate.info.sha256}</code>
            <p>{candidate.info.capabilities.map(item => item.id).join(", ")}</p>
            <button
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
          </div>
        )}
        <p>{t("安装后为服务选择版本并保存。每个服务只能启用同一插件的一个版本。")}</p>
      </section>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {inventory && (
        <>
          <details>
            <summary>{t("插件目录")}</summary>
            <code>{inventory.directory}</code>
            <p>{t("用户插件目录")}</p>
            <code>{inventory.managed_directory}</code>
          </details>
          {inventory.items.length === 0 && <p>{t("插件目录中没有动态库。")}</p>}
          <div className="settings-table">
            <table className="data-table" aria-label={t("原生插件目录")}>
              <thead>
                <tr>
                  <th>{t("插件")}</th>
                  <th>{t("版本")}</th>
                  <th>{t("能力")}</th>
                  <th>{t("状态")}</th>
                  <th>{t("管理")}</th>
                </tr>
              </thead>
              <tbody>
                {inventory.items.map(item => (
                  <tr key={`${item.managed}:${item.file}`}>
                    <td>
                      {item.id || item.file}
                      <details>
                        <summary>{t("详情")}</summary>
                        <code>{item.sha256}</code>
                        <p>{item.file}</p>
                      </details>
                    </td>
                    <td>{item.version || "—"}</td>
                    <td>
                      {item.capabilities.map(c => `${c.id} / v${c.version}`).join(", ") || "—"}
                    </td>
                    <td>
                      {item.state === "available" ? (
                        t("可用")
                      ) : (
                        <ErrorNotice error={new BackendError("unavailable", item.error)} />
                      )}
                    </td>
                    <td>
                      {item.managed ? (
                        <>
                          {removing === item.file ? (
                            <>
                              <p>{t("仅移除可部署副本；已部署服务、任务和历史数据保留。")}</p>
                              <button
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
                            <button
                              disabled={busy || installing || !item.sha256}
                              onClick={() => setRemoving(item.file)}
                            >
                              {t("卸载插件")}
                            </button>
                          )}
                        </>
                      ) : (
                        t("应用自带")
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          {services.map(service => (
            <ServicePlugins
              key={service.id}
              service={service}
              items={inventory.items}
              busy={busy}
              trade={trade}
              maintenance={node?.health?.maintenance ?? false}
              online={node?.state === "online"}
            />
          ))}
          {services.length === 0 && (
            <fieldset disabled={busy || node?.state !== "online" || node?.health?.maintenance}>
              <legend>{t("创建本机数据与任务服务")}</legend>
              <p>{t("应用自带的插件始终启用；自行安装的插件可以选择是否启用。")}</p>
              {availableDataTaskPlugins(inventory.items).map(item => (
                <PluginChoice
                  key={item.sha256}
                  item={item}
                  required={requiredPluginIds(inventory.items).has(item.id)}
                  checked={initialPlugins.includes(item.sha256)}
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
              <button
                onClick={async () => {
                  setError("");
                  try {
                    await trade("node.data_tasks.local.create", { plugins: initialPlugins });
                    setInitialPlugins([]);
                  } catch (reason) {
                    setError(asDisplayError(reason));
                  }
                }}
              >
                {t("创建数据与任务服务")}
              </button>
            </fieldset>
          )}
        </>
      )}
      <p>{t("分别管理本机数据服务和任务服务的原生插件；可复用凭据在连接页面配置。")}</p>
    </section>
  );
}
