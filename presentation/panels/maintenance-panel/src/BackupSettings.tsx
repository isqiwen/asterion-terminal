import { invoke } from "@asterion/desktop-bridge/native";
import { useEffect, useState } from "react";
import { nativeDesktop } from "@asterion/desktop-bridge/desktop";

type Verification = {
  versions: number;
  research_results: number;
  research_external_not_recomputed: number;
  quarantined_tasks: number;
};
type Backup = {
  status: string;
  path: string;
  bytes: number;
  sha256: string;
  verification: Verification;
};
type Restored = Verification & { status: string; target: string };
export function BackupSettings() {
  const [environment, setEnvironment] = useState<{
    active: string;
    previous: string | null;
    pending: string | null;
    protection_backup: string | null;
  } | null>(null);
  const [activationTarget, setActivationTarget] = useState("");
  useEffect(() => {
    if (nativeDesktop)
      void invoke<typeof environment>("desktop_environment")
        .then(setEnvironment)
        .catch((e) => setError(String(e)));
  }, []);
  async function activate(target: string | null) {
    setBusy("正在保护备份、重新验证并切换环境；完成后所有窗口重新登录…");
    setError("");
    try {
      await invoke("desktop_activate", { target });
      setEnvironment(await invoke<typeof environment>("desktop_environment"));
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy("");
    }
  }
  const [busy, setBusy] = useState("");
  const [error, setError] = useState("");
  const [backup, setBackup] = useState<Backup | null>(null);
  const [restored, setRestored] = useState<Restored | null>(null);
  const [archive, setArchive] = useState("");
  const [target, setTarget] = useState("");
  async function create() {
    setBusy("正在创建一致备份，后台服务会短暂停止；随后在独立目录验证恢复…");
    setError("");
    setBackup(null);
    try {
      const result = await invoke<Backup>("desktop_backup");
      setBackup(result);
      setArchive(result.path);
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy("");
    }
  }
  async function restore() {
    setBusy("正在校验备份并恢复到新目录，当前数据不会被覆盖…");
    setError("");
    setRestored(null);
    try {
      const result = await invoke<Restored>("desktop_restore", {
        archive,
        target,
      });
      setRestored(result);
      setActivationTarget(result.target);
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy("");
    }
  }
  return (
    <section aria-label="备份与恢复">
      <h2 className="settings-section-title">本机一致备份</h2>
      <p>
        保存数据库、行情文件、研究记录、账户和本机密钥。短暂停止后台后创建快照，恢复此前的服务状态，再实际验证独立数据库与数据文件。
      </p>
      <p>
        备份未加密，包含账户和敏感配置；限制为当前用户可读，请妥善保存。界面布局和浏览器偏好不在此备份范围。
      </p>
      <button disabled={!nativeDesktop || !!busy} onClick={() => void create()}>
        创建备份并验证
      </button>
      {!nativeDesktop && <p>请在桌面应用中操作备份与恢复。</p>}
      {backup && (
        <div role="status">
          <p>
            备份已验证 · {backup.verification.versions} 个数据版本 ·{" "}
            {backup.verification.research_results} 个研究结果
          </p>
          {backup.verification.research_external_not_recomputed > 0 && <p>
            {backup.verification.research_external_not_recomputed} 个外部策略结果仅验证完整性，未执行插件复算。
          </p>}
          <p className="mono">{backup.path}</p>
          <p className="mono">SHA-256：{backup.sha256}</p>
        </div>
      )}
      <h2 className="settings-section-title">恢复到新目录</h2>
      <p>
        仅恢复可信的本机备份；需要相同操作系统、CPU 架构和 PostgreSQL
        主版本。不会覆盖或切换当前工作台；恢复后的排队、运行任务会隔离为已取消，登录会话清除，需检查后显式重试。
      </p>
      <label>
        备份文件完整路径
        <input
          value={archive}
          disabled={!!busy}
          onChange={(e) => setArchive(e.target.value)}
        />
      </label>
      <label>
        新恢复目录完整路径
        <input
          value={target}
          disabled={!!busy}
          onChange={(e) => setTarget(e.target.value)}
          placeholder="必须是尚不存在的目录"
        />
      </label>
      <button
        disabled={!nativeDesktop || !!busy || !archive.trim() || !target.trim()}
        onClick={() => void restore()}
      >
        验证并恢复到新目录
      </button>
      {restored && (
        <div role="status">
          <p>
            恢复验证通过 · {restored.versions} 个数据版本 ·{" "}
            {restored.research_results} 个研究结果 ·{" "}
            {restored.quarantined_tasks} 项任务已隔离
          </p>
          {restored.research_external_not_recomputed > 0 && <p>
            {restored.research_external_not_recomputed} 个外部策略结果仅验证完整性，未执行插件复算。
          </p>}
          <p className="mono">{restored.target}</p>
          <p>当前工作台未切换。恢复目录中的后台未启动。</p>
        </div>
      )}
      <h2 className="settings-section-title">使用恢复环境</h2>
      <p>
        切换前停止后台并验证保护备份，再重新校验目标环境。请先保存界面编辑，成功后所有窗口重新登录；失败时尝试恢复原环境。切换和回滚都会取消目标环境尚未完成的任务，需检查后手动重试。
      </p>
      {environment && (
        <div>
          <p>
            当前环境：<span className="mono">{environment.active}</span>
          </p>
          {environment.protection_backup && (
            <p>
              最近保护备份：
              <span className="mono">{environment.protection_backup}</span>
            </p>
          )}
          {environment.pending && (
            <p role="alert">
              上次切换未完成，请在本机服务中重新启动以恢复原环境。
            </p>
          )}
        </div>
      )}
      <label>
        已恢复目录完整路径
        <input
          value={activationTarget}
          disabled={!!busy}
          onChange={(e) => setActivationTarget(e.target.value)}
        />
      </label>
      <button
        disabled={!nativeDesktop || !!busy || !activationTarget.trim()}
        onClick={() => void activate(activationTarget.trim())}
      >
        保护备份并切换
      </button>
      {environment?.previous && (
        <div>
          <p>
            上一个环境：<span className="mono">{environment.previous}</span>
          </p>
          <button disabled={!!busy} onClick={() => void activate(null)}>
            保护备份并回滚
          </button>
        </div>
      )}
      {busy && <p role="status">{busy}</p>}
      {error && <p role="alert">{error}</p>}
    </section>
  );
}
