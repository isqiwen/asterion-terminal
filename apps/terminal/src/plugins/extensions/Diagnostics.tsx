import { useState } from "react";
import type { RequestClient } from "../../api/requests";

type Execution = {
  id: string; started: number; duration_ms: number; phase: string;
  code: string; calls: number;
};
const reasons: Record<string, [string, string]> = {
  success: ["完成", ""],
  revoked: ["已停用", "检查插件是否启用，再重新发起操作。"],
  timeout: ["执行超时", "检查算法或网络请求是否阻塞，减少单次工作量。"],
  request_limit: ["请求过大", "减少单次请求的数据量。"],
  output_limit: ["输出超限", "减少返回数据和进程日志输出。"],
  process_exit: ["进程退出", "检查插件入口和依赖，并在开发环境运行同一工件。"],
  protocol: ["响应无效", "检查公共 SDK 协议；不要向标准输出打印调试信息。"],
  startup: ["启动失败", "检查本机运行时和插件文件的访问权限。"],
  host_error: ["宿主调用失败", "检查运行时和本机服务状态。"],
};
export function Diagnostics({ api, identifier }: { api: RequestClient; identifier: string }) {
  const [items, setItems] = useState<Execution[] | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  async function refresh() {
    setBusy(true); setError("");
    try {
      const result = await api.request<{ items: Execution[] }>(`/extensions/${identifier}/diagnostics`);
      setItems(result.items);
    } catch (e) { setError(String(e)); }
    finally { setBusy(false); }
  }
  return <section aria-label={`${identifier} 运行诊断`}>
    <button disabled={busy} onClick={() => void refresh()} title="读取当前工件最近 30 条调用记录">
      {busy ? "读取中…" : "运行诊断"}
    </button>
    {error && <p role="alert">诊断读取失败：{error}</p>}
    {items && <>
      <p className="settings-note">当前工件最近调用；仅记录进程执行，不代表业务结果已发布。无参数或原始日志。</p>
      {items.length === 0 && <p>暂无调用记录</p>}
      {items.length > 0 && <table>
        <thead><tr><th>时间</th><th>阶段</th><th>耗时</th><th>调用数</th><th>结果</th></tr></thead>
        <tbody>{items.map((item) => <tr key={item.id}>
          <td>{new Date(item.started * 1000).toLocaleString()}</td>
          <td>{item.phase}</td><td>{item.duration_ms} ms</td><td>{item.calls}</td>
          <td title={reasons[item.code]?.[1]}>{reasons[item.code]?.[0] ?? item.code}
            {item.code !== "success" && <small>{reasons[item.code]?.[1]}</small>}
          </td>
        </tr>)}</tbody>
      </table>}
    </>}
  </section>;
}
