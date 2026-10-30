import { useState } from "react";
import { request, type Snapshot, type BackupReport } from "../bridge/client";
import { translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { SettingsSection } from "./Section";
import { kindLabels } from "./service-state";
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
  const running = selected.filter(id =>
    services.some(item => item.id === id && (item.desired_running || item.pid)),
  );
  return (
    <div role="region" aria-label={t("数据备份与恢复")}>
      <SettingsSection
        title={t("新建备份")}
        description={t(
          "备份所选服务的完整数据目录，不包含运行状态。先在本机服务页停止所选服务。交易账户的记录在交易主机上，由交易主机备份。",
        )}
      >
        <fieldset disabled={busy} className="settings-list">
          <legend className="settings-hint">{t("选择本机业务数据")}</legend>
          {services.map(service => (
            <label className="settings-check" key={service.id}>
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
              {t(kindLabels[service.kind])} · {service.id}
              {(service.desired_running || service.pid) && ` · ${t("运行中")}`}
            </label>
          ))}
          {!services.length && <p className="settings-hint">{t("还没有本机数据或研究服务。")}</p>}
        </fieldset>
        <label className="settings-field">
          {t("新备份目录")}
          <input
            disabled={busy}
            placeholder={t("一个尚不存在的目录的绝对路径")}
            value={destination}
            onChange={event => {
              setDestination(event.target.value);
              setPlan(null);
              setConfirmSave(false);
            }}
          />
        </label>
        {running.length > 0 && (
          <p className="settings-hint" role="status">
            {t("所选服务仍在运行，先在本机服务页停止：")} {running.join("、")}
          </p>
        )}
        <div className="settings-actions">
          <button
            disabled={busy || !stopped || !destination}
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
        </div>
        {plan && (
          <div className="settings-card">
            <Report value={plan} />
            <label className="settings-check">
              <input
                type="checkbox"
                disabled={busy}
                checked={confirmSave}
                onChange={event => setConfirmSave(event.target.checked)}
              />
              {t("已核对所选范围、缺失依赖和新目录")}
            </label>
            <div className="settings-actions">
              <button
                className="primary"
                disabled={
                  busy ||
                  !stopped ||
                  !confirmSave ||
                  !destination ||
                  !plan.preview_digest ||
                  plan.saved
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
            </div>
          </div>
        )}
      </SettingsSection>
      <SettingsSection
        title={t("核验与恢复")}
        description={t(
          "核验已有备份，或把它恢复到一个新目录。恢复不覆盖原数据，也不恢复交易授权。",
        )}
      >
        <label className="settings-field">
          {t("已有备份目录")}
          <input
            disabled={busy}
            value={backupPath}
            onChange={event => {
              setBackupPath(event.target.value);
              setInspection(null);
              setConfirmRestore(false);
            }}
          />
        </label>
        <div className="settings-actions">
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
          <button
            disabled={busy || !backupPath}
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
        </div>
        {inspection && (
          <div className="settings-card">
            <Report value={inspection.value} />
            <label className="settings-field">
              {t("新恢复目录")}
              <input
                disabled={busy}
                value={restorePath}
                onChange={event => {
                  setRestorePath(event.target.value);
                  setConfirmRestore(false);
                }}
              />
            </label>
            <label className="settings-check">
              <input
                type="checkbox"
                disabled={busy}
                checked={confirmRestore}
                onChange={event => setConfirmRestore(event.target.checked)}
              />
              {t("确认恢复到独立新目录，不覆盖原数据或恢复授权")}
            </label>
            <div className="settings-actions">
              <button
                className="primary"
                disabled={busy || !restorePath || !confirmRestore || inspection.value.restored}
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
            </div>
          </div>
        )}
      </SettingsSection>
      {busy && (
        <div className="settings-item-row">
          <p role="status" className="settings-hint">
            {t("正在核验或复制业务数据…")}
          </p>
          <button
            onClick={() =>
              void request("system.backup.cancel", {}).catch(reason =>
                setError(asDisplayError(reason)),
              )
            }
          >
            {t("取消当前备份操作")}
          </button>
        </div>
      )}
      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </div>
  );
}
