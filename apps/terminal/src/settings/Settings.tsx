import { settingsCategories, useSettingsCategory } from "./category";
import { DataSources } from "./DataSources";
import {
  TerminalSecurity,
  SecurityPreferences,
} from "../account/TerminalSecurity";
import { useDesktopAccount } from "../account/useDesktopAccount";
import { AccountEntry } from "../account/AccountEntry";
import { useEffect, useState } from "react";
import { useConnection } from "../api/useConnection";
import { nativeDesktop, stopDesktop } from "../deployment/desktop";
import { savePreferences, usePreferences } from "./preferences";
import { Connection } from "../components/Connection";
export function Settings() {
  const [category, setCategory] = useSettingsCategory();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const connection = useConnection(false);
  const preferences = usePreferences();
  const { account, setAccount, restoring } = useDesktopAccount(
    connection.token,
  );
  useEffect(() => {
    document.title = "设置 · Asterion Terminal";
  }, []);
  async function toggleService() {
    setBusy(true);
    setError("");
    try {
      if (connection.connected) {
        await stopDesktop();
        connection.setToken("");
      } else {
        await connection.connect(true);
      }
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  if (nativeDesktop && restoring)
    return <div className="startup-screen">正在恢复桌面会话…</div>;
  if (nativeDesktop && !account)
    return <AccountEntry token={connection.token} onEnter={setAccount} />;
  return (
    <TerminalSecurity token={connection.token} account={account}>
      <div className="settings-window">
        <aside className="settings-sidebar">
          <div className="settings-brand">
            ✧ <b>Asterion</b>
          </div>
          <nav aria-label="设置分类">
            {settingsCategories.map((v) => (
              <button
                key={v}
                className={v === category ? "active" : ""}
                onClick={() => setCategory(v)}
              >
                {v}
              </button>
            ))}
          </nav>
          <span className="settings-hint">设置独立保存</span>
        </aside>
        <main className="settings-content">
          <h1>{category}</h1>
          {error && (
            <div role="alert" className="alert">
              {error}
            </div>
          )}
          {category === "安全" && <SecurityPreferences />}
          {category === "数据源" && <DataSources token={connection.token} />}
          {category === "外观" && (
            <>
              <h2 className="settings-section-title">界面</h2>
              <div className="setting-row">
                <div>
                  <label htmlFor="density">信息密度</label>
                  <small>调整表格行高与控件间距</small>
                </div>
                <select
                  id="density"
                  value={preferences.density}
                  onChange={(e) =>
                    savePreferences({
                      ...preferences,
                      density: e.target.value as "compact" | "comfortable",
                    })
                  }
                >
                  <option value="compact">紧凑</option>
                  <option value="comfortable">舒适</option>
                </select>
              </div>
              <div className="setting-row">
                <div>
                  <label htmlFor="colors">涨跌颜色</label>
                  <small>应用于行情图表</small>
                </div>
                <select
                  id="colors"
                  value={preferences.colors}
                  onChange={(e) =>
                    savePreferences({
                      ...preferences,
                      colors: e.target.value as "china" | "international",
                    })
                  }
                >
                  <option value="china">红涨绿跌</option>
                  <option value="international">绿涨红跌</option>
                </select>
              </div>
              <p className="settings-note">
                修改即时生效，工作区与已选对象保持不变。
              </p>
            </>
          )}
          {category === "本机服务" && (
            <>
              <h2 className="settings-section-title">运行环境</h2>
              <div className="setting-row">
                <div>
                  后台连接
                  <small>
                    {connection.starting
                      ? "正在读取状态"
                      : connection.connected
                        ? "本机服务可用"
                        : "本机服务未连接"}
                  </small>
                </div>
                <span className={connection.connected ? "good" : "muted"}>
                  ● {connection.connected ? "运行中" : "未连接"}
                </span>
              </div>
              <div className="setting-row directory-row">
                <div>
                  数据目录<small>{connection.directory || "尚未初始化"}</small>
                </div>
              </div>
              <p className="settings-note">
                关闭工作台窗口后，后台任务继续运行。停止服务会中断正在执行的任务，数据仍保留。
              </p>
              {nativeDesktop ? (
                <button
                  className="service-action"
                  disabled={busy || connection.starting}
                  onClick={toggleService}
                >
                  {busy
                    ? "正在处理…"
                    : connection.connected
                      ? "停止后台服务"
                      : "启动后台服务"}
                </button>
              ) : (
                <Connection onConnect={connection.setToken} />
              )}
            </>
          )}
          {category === "关于" && (
            <>
              <div className="about-mark">✧</div>
              <h2>Asterion Terminal</h2>
              <p>星衡 · 期货研究与交易工作台</p>
              <small>版本 0.1.0 · 数据功能基础版</small>
            </>
          )}
        </main>
      </div>
    </TerminalSecurity>
  );
}
