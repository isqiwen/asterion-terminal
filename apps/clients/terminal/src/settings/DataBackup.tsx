import { useState } from "react";
import { request, type Snapshot, type BackupReport } from "../bridge/client";
import { translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
const t = (key: string) => translate("host", key);

function Report({ value }: { value: BackupReport }) {
  return (
    <section aria-label={t("备份核验结果")}>
      <p role="status">
        {t(value.partial ? "部分备份：请核对缺失依赖与限制" : "所选数据范围核验通过")}
      </p>
      <p>
        {t("文件数量")} · {value.manifest.files.length}
      </p>
      <p>
        {t("载荷字节")} · {value.payload_bytes?.toLocaleString() ?? t("待统计")}
      </p>
      {value.available_bytes !== undefined && (
        <p>
          {t("目标可用字节")} · {value.available_bytes.toLocaleString()}
        </p>
      )}
      <ul>
        {value.manifest.owners.map((owner, index) => (
          <li key={`${owner.node}/${owner.domain}/${owner.service}`}>
            <code>
              {owner.node} / {owner.domain} / {owner.service}
            </code>
            <dl>
              {(value.manifest.scopes ?? [])
                .filter(field => field.owner === index)
                .map(field => (
                  <div key={field.key}>
                    <dt>
                      {field.key === "account_id"
                        ? t("账户标识")
                        : field.key === "environment"
                          ? t("账户环境")
                          : field.key}
                    </dt>
                    <dd>
                      {field.key === "environment" &&
                      (field.value === "real" || field.value === "simulation")
                        ? t(field.value === "real" ? "真实资金" : "CTP 仿真")
                        : field.value}
                    </dd>
                  </div>
                ))}
            </dl>
          </li>
        ))}
      </ul>
      <p>
        {t("未包含依赖")} · {value.manifest.dependencies.filter(item => !item.included).length}
      </p>
      <ul>
        {value.manifest.dependencies
          .filter(item => !item.included && item.object)
          .map(item => {
            const object = item.object!;
            return (
              <li
                key={`${object.owner.node}/${object.owner.service}/${object.kind}/${object.id}/${object.version}`}
              >
                <code style={{ overflowWrap: "anywhere" }}>
                  {object.owner.node} / {object.owner.domain} / {object.owner.service} /{" "}
                  {object.kind} / {object.id} / {object.version}
                </code>
              </li>
            );
          })}
      </ul>
      <ul>
        {value.manifest.limitations.map(item => (
          <li key={`${item.owner}/${item.reason}`}>
            {value.manifest.owners[item.owner].service} · {t(item.reason)}
          </li>
        ))}
      </ul>
      <p>
        {t("排除内容")} ·{" "}
        {[...new Set(value.manifest.exclusions.map(item => t(item.category)))].join(" · ")}
      </p>
      {value.saved && <p role="status">{t("备份已保存并核验")}</p>}
      {value.restored && <p role="status">{t("已恢复到独立目录并核验，未启动服务")}</p>}
    </section>
  );
}
export function DataBackup({ snapshot }: { snapshot: Snapshot | null }) {
  const node = snapshot?.nodes.find(item => item.id === "local");
  const services =
    node?.health?.services.filter(item => item.kind === "data" || item.kind === "research") ?? [];
  const [selected, setSelected] = useState<string[]>([]);
  const [destination, setDestination] = useState("");
  const [backupPath, setBackupPath] = useState("");
  const [restorePath, setRestorePath] = useState("");
  const [plan, setPlan] = useState<BackupReport | null>(null);
  const [inspection, setInspection] = useState<{ path: string; value: BackupReport } | null>(null);
  const [confirmSave, setConfirmSave] = useState(false);
  const [confirmRestore, setConfirmRestore] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const run = async (action: () => Promise<void>) => {
    setBusy(true);
    setError("");
    try {
      await action();
    } catch (reason) {
      setPlan(null);
      setConfirmSave(false);
      setConfirmRestore(false);
      setError(asDisplayError(reason));
    } finally {
      setBusy(false);
    }
  };
  const valueOf = (reply: Snapshot) => {
    if (!reply.backup) throw new Error(t("缺少备份核验结果"));
    return reply.backup;
  };
  const stopped =
    selected.length > 0 &&
    node?.state === "online" &&
    selected.every(id =>
      services.some(item => item.id === id && !item.desired_running && !item.pid),
    );
  return (
    <section className="settings-section" aria-label={t("数据备份与恢复")}>
      <p>
        {t(
          "先在本机服务停止所选本机 Data、Intelligence 和 Research。备份保留原记录，不恢复运行状态。交易账户的记录在交易主机的 data/ 目录，由交易主机自己备份。",
        )}
      </p>
      <fieldset disabled={busy}>
        <legend>{t("选择本机业务数据")}</legend>
        {services.map(service => (
          <label className="terminal-setting" key={service.id}>
            <input
              type="checkbox"
              checked={selected.includes(service.id)}
              onChange={event => {
                setSelected(held =>
                  event.target.checked
                    ? [...held, service.id]
                    : held.filter(id => id !== service.id),
                );
                setPlan(null);
                setConfirmSave(false);
              }}
            />
            {service.kind} · {service.id}
          </label>
        ))}
        <label className="terminal-setting">
          {t("新备份目录")}
          <input
            value={destination}
            onChange={event => {
              setDestination(event.target.value);
              setPlan(null);
              setConfirmSave(false);
            }}
          />
        </label>
        <button
          disabled={!stopped || !destination}
          onClick={() =>
            void run(async () => {
              setPlan(null);
              setConfirmSave(false);
              setPlan(
                valueOf(
                  await request("system.backup.preview", {
                    operation_id: crypto.randomUUID(),
                    services: selected,
                    destination,
                  }),
                ),
              );
            })
          }
        >
          {t("预览备份")}
        </button>
        {plan && (
          <>
            <Report value={plan} />
            <label>
              <input
                type="checkbox"
                checked={confirmSave}
                onChange={event => setConfirmSave(event.target.checked)}
              />
              {t("已核对所选范围、缺失依赖和新目录")}
            </label>
            <button
              disabled={
                !stopped || !confirmSave || !destination || !plan.preview_digest || plan.saved
              }
              onClick={() =>
                void run(async () => {
                  const operation = plan.manifest.backup_id;
                  setConfirmSave(false);
                  setPlan(
                    valueOf(
                      await request("system.backup.save", {
                        operation_id: operation,
                        services: selected,
                        destination,
                        preview_digest: plan.preview_digest!,
                      }),
                    ),
                  );
                })
              }
            >
              {t("保存备份")}
            </button>
          </>
        )}
      </fieldset>
      <fieldset disabled={busy}>
        <legend>{t("核验与独立恢复")}</legend>
        <label className="terminal-setting">
          {t("已有备份目录")}
          <input
            value={backupPath}
            onChange={event => {
              setBackupPath(event.target.value);
              setInspection(null);
              setConfirmRestore(false);
            }}
          />
        </label>
        <button
          disabled={!backupPath}
          onClick={() =>
            void run(async () => {
              setInspection(null);
              setConfirmRestore(false);
              setInspection({
                path: backupPath,
                value: valueOf(await request("system.backup.inspect", { backup: backupPath })),
              });
            })
          }
        >
          {t("核验备份")}
        </button>
        <button
          disabled={busy || !backupPath}
          onClick={() =>
            void run(async () => {
              setPlan(null);
              setConfirmSave(false);
              const prepared = valueOf(
                await request("system.backup.resume.preview", { backup: backupPath }),
              );
              setSelected(prepared.manifest.owners.map(owner => owner.service));
              setDestination(backupPath);
              setPlan(prepared);
            })
          }
        >
          {t("预览继续未完成备份")}
        </button>
        {inspection && (
          <>
            <Report value={inspection.value} />
            <label className="terminal-setting">
              {t("新恢复目录")}
              <input
                value={restorePath}
                onChange={event => {
                  setRestorePath(event.target.value);
                  setConfirmRestore(false);
                }}
              />
            </label>
            <label>
              <input
                type="checkbox"
                checked={confirmRestore}
                onChange={event => setConfirmRestore(event.target.checked)}
              />
              {t("确认恢复到独立新目录，不覆盖原数据或恢复授权")}
            </label>
            <button
              disabled={!restorePath || !confirmRestore || inspection.value.restored}
              onClick={() =>
                void run(async () => {
                  setConfirmRestore(false);
                  setInspection({
                    path: inspection.path,
                    value: valueOf(
                      await request("system.backup.restore", {
                        backup: inspection.path,
                        destination: restorePath,
                      }),
                    ),
                  });
                })
              }
            >
              {t("恢复到新目录")}
            </button>
          </>
        )}
      </fieldset>
      {busy && (
        <>
          <p role="status">{t("正在核验或复制业务数据…")}</p>
          <button
            onClick={() =>
              void request("system.backup.cancel", {}).catch(reason =>
                setError(asDisplayError(reason)),
              )
            }
          >
            {t("取消当前备份操作")}
          </button>
        </>
      )}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </section>
  );
}
