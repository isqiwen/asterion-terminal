import { useCallback, useEffect, useState, type ReactNode } from "react";
import {
  notify,
  ActionError,
  useActionError,
  lab,
  translate,
  type LabStrategies,
  type LabSummary,
  type MessageValues,
  type TerminalContext,
} from "../contract";
import { moduleTemplate } from "./research-lab";
import { SourceEditor } from "./SourceEditor";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

export type Folder = "strategies" | "factors";

// The modules of the research directory, read again after each change.
export function useModules(repository: string) {
  const [modules, setModules] = useState<LabStrategies | null>(null);
  const read = useCallback(async () => {
    if (!repository) return;
    try {
      setModules(await lab({ case: "listStrategies", value: { path: repository } }));
    } catch (failure) {
      notify(failure);
    }
  }, [repository]);
  useEffect(() => void read(), [read]);
  return { modules, read };
}

// The code commit and the research dataset version the next run will use.
export function LabHeader({
  context,
  modules,
  refresh,
}: {
  context: TerminalContext;
  modules: LabStrategies | null;
  refresh: () => void;
}) {
  const endpoint = context.snapshot?.history_endpoint;
  const address =
    endpoint && "host" in endpoint
      ? `${endpoint.host}:${endpoint.port}/${endpoint.fingerprint}`
      : "";
  const [summary, setSummary] = useState<LabSummary | null>(null);
  useEffect(() => {
    let active = true;
    setSummary(null);
    // Without a History service there is no research data to read; the chip says so.
    if (!address) return;
    void lab({ case: "getDatasetSummary", value: {} })
      .then(value => active && setSummary(value))
      .catch(failure => active && notify(failure));
    return () => {
      active = false;
    };
  }, [address]);
  return (
    <header className="lab-header">
      {modules?.commit && <code className="lab-chip">{modules.commit.slice(0, 8)}</code>}
      {modules?.changes && <span className="lab-chip warn">{t("有未提交改动")}</span>}
      <button className="lab-chip" onClick={() => context.navigate("workspace.coverage", {})}>
        {t("研究数据")} ·{" "}
        {address ? (
          <code>{summary?.dataset?.version.slice(0, 8) || t("无数据")}</code>
        ) : (
          t("未设置")
        )}
      </button>
      <span className="lab-spacer" />
      <button className="lab-link" onClick={refresh}>
        {t("刷新")}
      </button>
    </header>
  );
}

// A new module of strategies/ or factors/, named in place and opened at once.
export function NewModule({
  folder,
  repository,
  created,
}: {
  folder: Folder;
  repository: string;
  created: (path: string) => void;
}) {
  const [naming, setNaming] = useState(false);
  const [name, setName] = useState("");
  const { error, attempt, clear } = useActionError();
  if (!naming)
    return (
      <button className="lab-link" onClick={() => setNaming(true)}>
        {t(folder === "strategies" ? "新建策略" : "新建因子")}
      </button>
    );
  return (
    <form
      className="lab-new-module"
      onSubmit={event => {
        event.preventDefault();
        const path = `${folder}/${name}.py`;
        void attempt(async () => {
          await lab({
            case: "saveSource",
            value: {
              source: { repository, path },
              content: moduleTemplate(folder, name),
              create: true,
            },
          });
          setNaming(false);
          setName("");
          created(path);
        });
      }}
    >
      <input
        aria-label={t("模块名")}
        placeholder={folder === "strategies" ? "my_strategy" : "my_factor"}
        pattern="[A-Za-z_][A-Za-z0-9_]*"
        required
        autoFocus
        value={name}
        onChange={event => setName(event.target.value)}
      />
      <button className="primary" type="submit">
        {t("创建")}
      </button>
      <button
        type="button"
        onClick={() => {
          setNaming(false);
          clear();
        }}
      >
        {t("取消")}
      </button>
      <ActionError error={error} />
    </form>
  );
}

// Modules of one folder that failed to import. Lab names a factor module
// "factors/<name>" and a strategy module by its bare name.
export function ModuleErrors({
  folder,
  modules,
  open,
}: {
  folder: Folder;
  modules: LabStrategies | null;
  open: (path: string) => void;
}) {
  return modules?.errors
    .filter(item => item.module.startsWith("factors/") === (folder === "factors"))
    .map(item => (
      <details key={item.module} className="lab-module-error">
        <summary>
          {item.module}.py · {t("导入失败")}
        </summary>
        <pre>{item.traceback}</pre>
        <button
          className="lab-link"
          onClick={() =>
            open(folder === "factors" ? `${item.module}.py` : `strategies/${item.module}.py`)
          }
        >
          {t("打开代码")}
        </button>
      </details>
    ));
}

// The open module and whether the centre shows its code or the latest result,
// which takes over whenever a new one arrives.
export function useSource(shown: unknown) {
  const [path, setPath] = useState("");
  const [tab, setTab] = useState<"code" | "result">("code");
  useEffect(() => {
    if (shown) setTab("result");
  }, [shown]);
  return {
    path,
    tab,
    setTab,
    open: (next: string) => {
      setPath(next);
      setTab("code");
    },
    close: () => setPath(""),
  };
}

export function LabCentre({
  source,
  repository,
  hasResult,
  changed,
  deleted,
  children,
}: {
  source: ReturnType<typeof useSource>;
  repository: string;
  hasResult: boolean;
  changed: () => void;
  deleted: () => void;
  children: ReactNode;
}) {
  return (
    <main className="lab-center">
      {source.path && hasResult && (
        <nav className="lab-tabs" aria-label={t("代码与结果")}>
          <button aria-pressed={source.tab === "code"} onClick={() => source.setTab("code")}>
            {t("代码")}
          </button>
          <button aria-pressed={source.tab === "result"} onClick={() => source.setTab("result")}>
            {t("结果")}
          </button>
        </nav>
      )}
      {source.path && source.tab === "code" ? (
        <SourceEditor
          key={source.path}
          repository={repository}
          path={source.path}
          changed={changed}
          deleted={() => {
            source.close();
            deleted();
          }}
        />
      ) : (
        children
      )}
    </main>
  );
}
