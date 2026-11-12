import { useCallback, useEffect, useState, type ReactNode } from "react";
import { lab } from "../../src/bridge/lab";
import type { Strategies } from "../../src/generated/asterion/lab_pb";
import type { DatasetSummary } from "../../src/generated/asterion/warehouse_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { notify } from "../../src/host/notifications";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { type MessageValues, translate } from "../../src/i18n";
import { moduleTemplate } from "./research-lab";
import { SourceEditor } from "./SourceEditor";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

export type Folder = "strategies" | "factors";

// The modules of the research directory, read again after each change.
export function useModules(repository: string) {
  const [modules, setModules] = useState<Strategies | null>(null);
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
  modules: Strategies | null;
  refresh: () => void;
}) {
  const endpoint = context.snapshot?.warehouse_endpoint;
  const address =
    endpoint && "host" in endpoint
      ? `${endpoint.host}:${endpoint.port}/${endpoint.fingerprint}`
      : "";
  const [summary, setSummary] = useState<DatasetSummary | null>(null);
  useEffect(() => {
    let active = true;
    setSummary(null);
    // Without a Warehouse service there is no research data to read; the chip says so.
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
  modules: Strategies | null;
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

// The open module, its unsaved edits and whether the centre shows its code or
// the latest result, which takes over whenever a new one arrives. The edits
// belong to the workspace: they outlive the editor until saved.
export function useSource(repository: string, shown: unknown) {
  const [path, setPath] = useWorkspaceDraft(`source-path:${repository}`, "");
  const [drafts, setDrafts] = useWorkspaceDraft<Record<string, string>>(
    `source-drafts:${repository}`,
    {},
  );
  const [tab, setTab] = useState<"code" | "result">("code");
  useEffect(() => {
    if (shown) setTab("result");
  }, [shown]);
  const draft = (module: string, content: string | null) =>
    setDrafts(previous => {
      const { [module]: _, ...rest } = previous;
      return content === null ? rest : { ...rest, [module]: content };
    });
  return {
    path,
    tab,
    setTab,
    drafts,
    draft,
    open: (next: string) => {
      setPath(next);
      setTab("code");
    },
    close: () => setPath(""),
    // Saves every module with unsaved edits, so that a run reads the code on show; the paths saved.
    async save() {
      const saved = Object.keys(drafts);
      for (const module of saved) {
        await lab({
          case: "saveSource",
          value: { source: { repository, path: module }, content: drafts[module] },
        });
        draft(module, null);
      }
      return saved;
    },
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
        <nav className="lab-tabs tabs" aria-label={t("代码与结果")}>
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
          draft={source.drafts[source.path]}
          edited={content => source.draft(source.path, content)}
          changed={changed}
          deleted={() => {
            source.draft(source.path, null);
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

// The configuration column folds to a strip so a result can use the width.
export function useConfigFold() {
  return useWorkspaceDraft("lab-config-folded", false);
}
export function ConfigFold({ folded, fold }: { folded: boolean; fold: (folded: boolean) => void }) {
  return (
    <button
      className="lab-fold"
      aria-expanded={!folded}
      aria-label={t(folded ? "展开配置" : "收起配置")}
      title={t(folded ? "展开配置" : "收起配置")}
      onClick={() => fold(!folded)}
    >
      {folded ? "‹" : "›"}
    </button>
  );
}
