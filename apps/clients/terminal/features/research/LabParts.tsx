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
export function LabStatus({
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
    <footer className="lab-status">
      {modules?.commit && <code className="lab-chip">{modules.commit.slice(0, 8)}</code>}
      {modules?.changes && <span className="lab-chip warn">{t("有未提交改动")}</span>}
      <button className="lab-chip" onClick={() => context.navigate("workspace.data-overview", {})}>
        {t("研究数据")} ·{" "}
        {address ? (
          <code>{summary?.dataset?.version.slice(0, 8) || t("无数据")}</code>
        ) : (
          t("未设置")
        )}
      </button>
      <button className="lab-link" onClick={refresh}>
        {t("刷新")}
      </button>
    </footer>
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
        disabled={!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)}
        onClick={() => {
          const path = `${folder}/${name}.py`;
          setNaming(false);
          setName("");
          clear();
          created(path);
        }}
      >
        {t("打开")}
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
export function useSource(repository: string, folder: "strategies" | "factors" = "strategies") {
  const [path, setPath] = useWorkspaceDraft(`source-path:${folder}:${repository}`, "");
  const [drafts, setDrafts] = useWorkspaceDraft<Record<string, string>>(
    `source-drafts:${repository}`,
    {},
  );
  const draft = (module: string, content: string | null) =>
    setDrafts(previous => {
      const { [module]: _, ...rest } = previous;
      return content === null ? rest : { ...rest, [module]: content };
    });
  return {
    path,
    drafts,
    draft,
    open: setPath,
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
  changed,
  deleted,
}: {
  source: ReturnType<typeof useSource>;
  repository: string;
  changed: () => void;
  deleted: () => void;
}) {
  return (
    <main className="lab-center">
      {source.path && (
        <SourceEditor
          key={source.path}
          repository={repository}
          path={source.path}
          draft={source.drafts[source.path]}
          edited={content => source.draft(source.path, content)}
          changed={changed}
          opened={source.open}
          deleted={() => {
            source.draft(source.path, null);
            source.close();
            deleted();
          }}
        />
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

// Deleting a record takes a second click; its results and code copy go with it.
export function DeleteRecord({ name, remove }: { name: string; remove: () => void }) {
  const [armed, setArmed] = useState(false);
  return armed ? (
    <button className="danger" onClick={remove} onBlur={() => setArmed(false)}>
      {t("确认删除 {name}", { name })}
    </button>
  ) : (
    <button
      className="lab-link"
      aria-label={t("删除 {name}", { name })}
      onClick={() => setArmed(true)}
    >
      {t("删除")}
    </button>
  );
}

// One group of a configuration; its summary says what is set while folded.
export function Section({
  title,
  summary,
  open,
  children,
}: {
  title: string;
  summary: string;
  open?: boolean;
  children: ReactNode;
}) {
  return (
    <details className="lab-section" open={open}>
      <summary>
        <strong>{title}</strong>
        <small title={summary}>{summary}</small>
      </summary>
      <div className="lab-form">{children}</div>
    </details>
  );
}

// One strategy or factor in a module list.
export type ModuleItem = {
  id: string;
  name: string;
  module: string;
  description: string;
  // Bar fields a factor takes; the list filters by them.
  inputs?: readonly string[];
  // Its latest result, on the right of its row.
  figure?: { text: string; className?: string; title: string };
};

// Strategies or factors grouped by module, one line each, searchable; a group
// folds. Many modules stay readable: the description shows on hover.
export function ModuleList({
  folder,
  items,
  chosen,
  choose,
}: {
  folder: Folder;
  items: readonly ModuleItem[];
  chosen: string;
  choose: (item: ModuleItem) => void;
}) {
  const [search, setSearch] = useWorkspaceDraft(`modules-search:${folder}`, "");
  const [input, setInput] = useWorkspaceDraft(`modules-input:${folder}`, "");
  const [folded, setFolded] = useWorkspaceDraft<string[]>(`modules-folded:${folder}`, []);
  const inputs = [...new Set(items.flatMap(item => item.inputs ?? []))].sort();
  const query = search.trim().toLowerCase();
  const shown = items.filter(
    item =>
      (!input || item.inputs?.includes(input)) &&
      (!query || `${item.name} ${item.module} ${item.description}`.toLowerCase().includes(query)),
  );
  const modules = [...new Set(shown.map(item => item.module))].sort();
  return (
    <div className="lab-module-list">
      <input
        type="search"
        aria-label={t(folder === "strategies" ? "搜索策略" : "搜索因子")}
        placeholder={t(folder === "strategies" ? "搜索策略" : "搜索因子")}
        value={search}
        onChange={event => setSearch(event.target.value)}
      />
      {inputs.length > 1 && (
        <select
          aria-label={t("输入数据")}
          value={input}
          onChange={event => setInput(event.target.value)}
        >
          <option value="">{t("全部输入")}</option>
          {inputs.map(item => (
            <option key={item}>{item}</option>
          ))}
        </select>
      )}
      {!shown.length && items.length > 0 && <p className="subtle">{t("没有匹配的模块")}</p>}
      {modules.map(module => {
        // A search opens every group it matches in.
        const open = !!query || !folded.includes(module);
        const members = shown.filter(item => item.module === module);
        return (
          <section key={module} aria-label={module}>
            <button
              className="lab-module-group"
              aria-expanded={open}
              onClick={() =>
                setFolded(previous =>
                  previous.includes(module)
                    ? previous.filter(item => item !== module)
                    : [...previous, module],
                )
              }
            >
              <span>{module}</span>
              <small>{members.length}</small>
            </button>
            {open && (
              <ul>
                {members.map(item => (
                  <li key={item.id}>
                    <button
                      aria-pressed={item.id === chosen}
                      title={item.description}
                      onClick={() => choose(item)}
                    >
                      <span>{item.name}</span>
                      {item.figure && (
                        <small className={item.figure.className} title={item.figure.title}>
                          {item.figure.text}
                        </small>
                      )}
                    </button>
                  </li>
                ))}
              </ul>
            )}
          </section>
        );
      })}
    </div>
  );
}
