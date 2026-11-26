import {
  lazy,
  Suspense,
  useCallback,
  useEffect,
  useId,
  useLayoutEffect,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { lab } from "../../src/bridge/lab";
import type { Strategies } from "../../src/generated/asterion/lab_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import { notify } from "../../src/host/notifications";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { type MessageValues, translate } from "../../src/i18n";
import { Icon } from "../../src/ui/Icon";
import {
  readAnnotations,
  saveAnnotation,
  subscribeAnnotations,
  type ExperimentAnnotation,
} from "./research-storage";
import { copiedModule, copyStem, moduleTemplate, strategyClassName } from "./research-templates";

const SourceEditor = lazy(() =>
  import("./SourceEditor").then(module => ({ default: module.SourceEditor })),
);

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

export type Folder = "strategies" | "factors";
export type ModuleCopy = { name: string; content: string; symbol?: string };
export type ModuleDraft = {
  title: string;
  name: string;
  build: (name: string) => { content: string; id: string };
};

// The modules of the research directory, read again after each change or focus.
export function useModules(repository: string) {
  const [modules, setModules] = useState<Strategies | null>(null);
  const read = useCallback(async () => {
    if (!repository) return;
    try {
      setModules(await lab({ case: "listStrategies", value: { path: repository } }));
    } catch (failure) {
      notify(failure, { namespace: "asterion.terminal.research" });
    }
  }, [repository]);
  const forget = useCallback((path: string) => {
    const isFactor = path.startsWith("factors/");
    const stem = path.replace(/^(strategies|factors)\//, "").replace(/\.py$/, "");
    setModules(previous =>
      previous
        ? {
            ...previous,
            strategies: isFactor
              ? previous.strategies
              : previous.strategies.filter(item => !item.id.startsWith(`${stem}.`)),
            factors: isFactor
              ? previous.factors.filter(item => !item.id.startsWith(`${stem}.`))
              : previous.factors,
            errors: previous.errors.filter(
              item => item.module !== (isFactor ? `factors/${stem}` : stem),
            ),
          }
        : previous,
    );
  }, []);
  useEffect(() => {
    void read();
    const onFocus = () => void read();
    window.addEventListener("focus", onFocus);
    return () => window.removeEventListener("focus", onFocus);
  }, [read]);
  return { modules, read, forget };
}

// A new module of strategies/ or factors/, named in a dialog and opened at once.
export function NewModule({
  folder,
  repository,
  copying,
  cancelCopy,
  generated,
  cancelGenerated,
  hideTrigger,
  created,
}: {
  folder: Folder;
  repository: string;
  copying?: ModuleCopy | null;
  cancelCopy?: () => void;
  generated?: ModuleDraft | null;
  cancelGenerated?: () => void;
  hideTrigger?: boolean;
  created: (path: string, id?: string) => void;
}) {
  const [naming, setNaming] = useState(false);
  const open = naming || Boolean(copying) || Boolean(generated);
  const close = () => {
    setNaming(false);
    cancelCopy?.();
    cancelGenerated?.();
  };
  return (
    <>
      {!hideTrigger && (
        <button type="button" className="lab-add-module" onClick={() => setNaming(true)}>
          <Icon name="plus" size={14} />
          <span>{t(folder === "strategies" ? "新建策略" : "新建因子")}</span>
        </button>
      )}
      {open && (
        <NewModuleDialog
          folder={folder}
          repository={repository}
          copying={copying}
          generated={generated}
          close={close}
          created={created}
        />
      )}
    </>
  );
}

function NewModuleDialog({
  folder,
  repository,
  copying,
  generated,
  close,
  created,
}: {
  folder: Folder;
  repository: string;
  copying?: ModuleCopy | null;
  generated?: ModuleDraft | null;
  close: () => void;
  created: (path: string, id?: string) => void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const heading = useId();
  const inputRef = useRef<HTMLInputElement>(null);
  const [name, setName] = useState(generated?.name ?? copying?.name ?? "");
  const { error, attempt } = useActionError();
  useLayoutEffect(() => {
    const element = dialog.current;
    element?.showModal();
    return () => element?.close();
  }, []);
  useEffect(() => {
    inputRef.current?.focus();
    inputRef.current?.select();
  }, []);
  const title = generated
    ? generated.title
    : copying
      ? folder === "strategies"
        ? "复制策略"
        : "复制因子"
      : folder === "strategies"
        ? "新建策略"
        : "新建因子";
  return (
    <dialog
      ref={dialog}
      className="lab-module-dialog"
      aria-labelledby={heading}
      onCancel={event => {
        event.preventDefault();
        close();
      }}
    >
      <h2 id={heading}>{t(title)}</h2>
      <form
        onKeyDown={event => {
          if (event.key === "Escape") {
            event.preventDefault();
            close();
          }
        }}
        onSubmit={event => {
          event.preventDefault();
          const path = `${folder}/${name}.py`;
          void attempt(async () => {
            const result = generated
              ? generated.build(name)
              : copying
                ? copiedModule(folder, copying.content, copying.symbol, name)
                : {
                    content: moduleTemplate(folder, name),
                    id: `${name}.${folder === "strategies" ? strategyClassName(name) : name}`,
                  };
            await lab({
              case: "saveSource",
              value: {
                source: { repository, path },
                content: result.content,
                create: true,
              },
            });
            close();
            created(path, result.id);
          });
        }}
      >
        <label>
          <span>{t("模块名")}</span>
          <input
            ref={inputRef}
            placeholder={folder === "strategies" ? "my_strategy" : "my_factor"}
            pattern="[A-Za-z_][A-Za-z0-9_]*"
            spellCheck={false}
            autoCorrect="off"
            autoCapitalize="none"
            required
            value={name}
            onChange={event => setName(event.target.value)}
          />
        </label>
        <ActionError error={error} namespace="asterion.terminal.research" />
        <footer>
          <button type="button" onClick={close}>
            {t("取消")}
          </button>
          <button className="primary" type="submit">
            {t("创建")}
          </button>
        </footer>
      </form>
    </dialog>
  );
}

// Modules of one folder that failed to import. Lab names a factor module
// "factors/<name>" and a strategy module by its bare name.
export function ModuleErrors({
  folder,
  modules,
  contextTarget,
  open,
  onMenu,
}: {
  folder: Folder;
  modules: Strategies | null;
  contextTarget?: string;
  open: (path: string) => void;
  onMenu?: (path: string, x: number, y: number, symbol?: string, target?: string) => void;
}) {
  return modules?.errors
    .filter(item => item.module.startsWith("factors/") === (folder === "factors"))
    .map(item => {
      const path = folder === "factors" ? `${item.module}.py` : `strategies/${item.module}.py`;
      return (
        <details key={item.module} className="lab-module-error">
          <summary
            data-context={contextTarget === path || undefined}
            onContextMenu={
              onMenu
                ? event => {
                    event.preventDefault();
                    onMenu(path, event.clientX, event.clientY, undefined, path);
                  }
                : undefined
            }
          >
            {item.module}.py · {t("导入失败")}
          </summary>
          <pre>{item.traceback}</pre>
          <button type="button" className="lab-link" onClick={() => open(path)}>
            {t("打开代码")}
          </button>
        </details>
      );
    });
}

// The open module, its unsaved edits and whether the centre shows its code or
// the latest result, which takes over whenever a new one arrives. The edits
// belong to the workspace: they outlive the editor until saved.
export function useSource(repository: string, folder: "strategies" | "factors" = "strategies") {
  const [path, setPath] = useWorkspaceDraft(`source-path:${folder}:${repository}`, "");
  const [history, setHistory] = useState<string[]>([]);
  const [drafts, setDrafts] = useWorkspaceDraft<Record<string, string>>(
    `source-drafts:${repository}`,
    {},
  );
  const pending = useRef<Record<string, string | null>>({});
  const timer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const flush = useCallback(() => {
    clearTimeout(timer.current);
    timer.current = undefined;
    const entries = Object.entries(pending.current);
    if (!entries.length) return;
    pending.current = {};
    setDrafts(previous => {
      const next = { ...previous };
      for (const [module, content] of entries) {
        if (content === null) delete next[module];
        else next[module] = content;
      }
      return next;
    });
  }, [setDrafts]);
  useEffect(() => () => flush(), [flush]);
  const draft = (module: string, content: string | null) => {
    pending.current[module] = content;
    clearTimeout(timer.current);
    if (content === null) flush();
    else timer.current = setTimeout(flush, 200);
  };
  const saved = (module: string, content: string) => {
    flush();
    setDrafts(previous => {
      if (previous[module] !== content) return previous;
      const next = { ...previous };
      delete next[module];
      return next;
    });
  };
  const back = history.at(-1);
  return {
    path,
    back,
    drafts,
    draftOf: (module: string) =>
      module in pending.current ? (pending.current[module] ?? undefined) : drafts[module],
    draft,
    saved,
    open: (next: string) => {
      flush();
      setHistory([]);
      setPath(next);
    },
    jump: (next: string) => {
      if (!next || next === path) return;
      flush();
      if (path) setHistory(previous => [...previous, path]);
      setPath(next);
    },
    goBack: () => {
      if (!back) return;
      flush();
      setHistory(previous => previous.slice(0, -1));
      setPath(back);
    },
    forget: (module: string) => {
      draft(module, null);
      setHistory(previous => previous.filter(item => item !== module));
      if (path === module) setPath("");
    },
    close: () => {
      flush();
      setHistory([]);
      setPath("");
    },
    // Save the run's captured drafts; edits made while saving stay in the workspace.
    async save() {
      const merged = { ...drafts };
      for (const [module, content] of Object.entries(pending.current)) {
        if (content === null) delete merged[module];
        else merged[module] = content;
      }
      flush();
      const modules = Object.keys(merged);
      for (const module of modules) {
        await lab({
          case: "saveSource",
          value: { source: { repository, path: module }, content: merged[module] },
        });
        saved(module, merged[module]);
      }
      return modules;
    },
  };
}

export function LabCentre({
  source,
  repository,
  modules,
  focus = "",
  changed,
  onDuplicate,
  onDelete,
}: {
  source: ReturnType<typeof useSource>;
  repository: string;
  modules?: Strategies | null;
  // The chosen `<module>.<symbol>`, shown when its module is open.
  focus?: string;
  changed: () => void;
  onDuplicate?: (path: string, content: string, symbol?: string) => void;
  onDelete?: (path: string) => void;
}) {
  const [focusModule, symbol] = focus.split(".");
  const focusedSymbol = source.path.endsWith(`/${focusModule}.py`) ? symbol : undefined;
  const traceback = modules?.errors.find(
    item =>
      (item.module.startsWith("factors/")
        ? `${item.module}.py`
        : `strategies/${item.module}.py`) === source.path,
  )?.traceback;
  return (
    <div className="lab-center">
      {source.path && (
        <Suspense fallback={null}>
          <SourceEditor
            key={`${repository}:${source.path}`}
            repository={repository}
            path={source.path}
            draft={source.drafts[source.path]}
            traceback={traceback}
            symbol={focusedSymbol}
            edited={content => source.draft(source.path, content)}
            saved={content => source.saved(source.path, content)}
            changed={changed}
            opened={source.jump}
            back={source.back}
            onBack={source.goBack}
            onDuplicate={
              onDuplicate ? content => onDuplicate(source.path, content, focusedSymbol) : undefined
            }
            onDelete={onDelete ? () => onDelete(source.path) : undefined}
          />
        </Suspense>
      )}
    </div>
  );
}

// The configuration column folds to a strip so a result can use the width.
export function useConfigFold() {
  return useWorkspaceDraft("lab-config-folded", false);
}
export function ConfigFold({ folded, fold }: { folded: boolean; fold: (folded: boolean) => void }) {
  return (
    <button
      type="button"
      className="lab-fold"
      aria-expanded={!folded}
      aria-label={t(folded ? "展开配置" : "收起配置")}
      title={t(folded ? "展开配置" : "收起配置")}
      onClick={() => fold(!folded)}
    >
      <Icon name={folded ? "chevronLeft" : "chevronRight"} size={14} />
    </button>
  );
}

// Deleting a record takes a second click; its results and code copy go with it.
export function DeleteRecord({ name, remove }: { name: string; remove: () => void }) {
  const [armed, setArmed] = useState(false);
  return (
    <button
      type="button"
      className={armed ? "danger" : "lab-link lab-delete"}
      aria-label={armed ? undefined : t("删除 {name}", { name })}
      onClick={() => {
        if (!armed) {
          setArmed(true);
          return;
        }
        setArmed(false);
        remove();
      }}
      onBlur={() => setArmed(false)}
    >
      {armed ? t("确认删除 {name}", { name }) : t("删除")}
    </button>
  );
}

// One group of a configuration.
export function Section({
  title,
  open,
  children,
}: {
  title: string;
  open?: boolean;
  children: ReactNode;
}) {
  return (
    <details className="lab-section" open={open}>
      <summary>
        <strong>{title}</strong>
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
  // Its type, which the strategy library filters by.
  group?: string;
  description: string;
  // Bar fields a factor takes, shown in the library and searchable.
  inputs?: readonly string[];
};

// A library table's sortable figures: missing values last, each sort announced.
export function useLibrarySort<Key extends string>(initial: Key) {
  const [sort, setSort] = useState({ key: initial, descending: true });
  const [announcement, setAnnouncement] = useState("");
  const heading = (key: Key, label: string, title?: string) => (
    <th
      key={key}
      scope="col"
      className="numeric"
      title={title}
      aria-sort={sort.key === key ? (sort.descending ? "descending" : "ascending") : undefined}
    >
      <button
        type="button"
        className="lab-sort"
        onClick={() => {
          const descending = sort.key === key ? !sort.descending : true;
          setSort({ key, descending });
          setAnnouncement(
            t("已按 {column}{direction}排序", {
              column: label,
              direction: descending ? t("降序") : t("升序"),
            }),
          );
        }}
      >
        {label}
        <span aria-hidden="true">{sort.key === key ? (sort.descending ? " ↓" : " ↑") : ""}</span>
      </button>
    </th>
  );
  const sorted = <Row,>(rows: readonly Row[], value: (row: Row, key: Key) => number | undefined) =>
    [...rows].sort((a, b) => {
      const [x, y] = [value(a, sort.key), value(b, sort.key)];
      if (x === undefined || y === undefined)
        return x === undefined ? (y === undefined ? 0 : 1) : -1;
      return sort.descending ? y - x : x - y;
    });
  const status = (
    <p className="sr-only" role="status">
      {announcement}
    </p>
  );
  return { heading, sorted, status };
}

// Strategies or factors by name, narrowed by a search.
export function filterModuleItems(items: readonly ModuleItem[], search: string) {
  const query = search.trim().toLowerCase();
  return items
    .filter(
      item =>
        !query ||
        `${item.name} ${item.module} ${item.description} ${(item.inputs ?? []).join(" ")}`
          .toLowerCase()
          .includes(query),
    )
    .sort((a, b) => a.name.localeCompare(b.name) || a.module.localeCompare(b.module));
}

// Context menu on a module row: duplicate any module, or delete a writable one.
export function useModuleMenu({
  repository,
  source,
  deleted,
}: {
  repository: string;
  source: ReturnType<typeof useSource>;
  deleted: (path: string) => void;
}) {
  const [menu, setMenu] = useState<{
    id: number;
    path: string;
    x: number;
    y: number;
    symbol?: string;
    target?: string;
    content?: string;
    readOnly?: boolean;
  } | null>(null);
  const [copying, setCopying] = useState<(ModuleCopy & { id: number }) | null>(null);
  const [armed, setArmed] = useState(false);
  const ref = useRef<HTMLDivElement>(null);
  const requestRef = useRef(0);
  useEffect(() => {
    if (!menu) return;
    const dismiss = (event: MouseEvent) => {
      if (!ref.current?.contains(event.target as Node)) setMenu(null);
    };
    const keydown = (event: KeyboardEvent) => {
      if (event.key === "Escape") setMenu(null);
    };
    const close = () => setMenu(null);
    document.addEventListener("mousedown", dismiss);
    document.addEventListener("keydown", keydown);
    window.addEventListener("blur", close);
    window.addEventListener("scroll", close, true);
    return () => {
      document.removeEventListener("mousedown", dismiss);
      document.removeEventListener("keydown", keydown);
      window.removeEventListener("blur", close);
      window.removeEventListener("scroll", close, true);
    };
  }, [menu]);
  const open = (path: string, x: number, y: number, symbol?: string, target?: string) => {
    const id = ++requestRef.current;
    setArmed(false);
    setMenu({ id, path, x, y, symbol, target });
    void lab({ case: "readSource", value: { repository, path } })
      .then(fetched => {
        setMenu(previous =>
          previous?.id === id
            ? {
                ...previous,
                content: source.draftOf(path) ?? fetched.content,
                readOnly: fetched.readOnly,
              }
            : previous,
        );
      })
      .catch(failure => {
        if (requestRef.current === id) setMenu(null);
        notify(failure, { namespace: "asterion.terminal.research" });
      });
  };
  const copy = (path: string, content: string, symbol?: string) => {
    setCopying({
      id: ++requestRef.current,
      name: copyStem(symbol, path),
      content,
      symbol,
    });
  };
  const remove = (path: string) => {
    void lab({ case: "deleteSource", value: { repository, path } })
      .then(() => {
        source.forget(path);
        deleted(path);
      })
      .catch(failure => notify(failure, { namespace: "asterion.terminal.research" }));
  };
  const overlay = menu && (
    <div
      ref={ref}
      className="lab-context-menu"
      role="menu"
      aria-label={menu.path}
      style={{
        left: Math.max(8, Math.min(menu.x, window.innerWidth - (armed ? 320 : 180))),
        top: Math.max(8, Math.min(menu.y, window.innerHeight - 100)),
      }}
    >
      <button
        type="button"
        role="menuitem"
        disabled={menu.content === undefined}
        onClick={() => {
          copy(menu.path, menu.content!, menu.symbol);
          setMenu(null);
        }}
      >
        <Icon name="copy" size={14} />
        <span>{t("复制")}</span>
      </button>
      {menu.readOnly === false && (
        <>
          <hr />
          <button
            type="button"
            role="menuitem"
            className="danger-item"
            onClick={() => {
              if (!armed) {
                setArmed(true);
                return;
              }
              const target = menu.path;
              setMenu(null);
              setArmed(false);
              remove(target);
            }}
          >
            <Icon name="trash" size={14} />
            <span>{armed ? t("确认删除 {name}", { name: menu.path }) : t("删除")}</span>
          </button>
        </>
      )}
    </div>
  );
  return {
    open,
    copy,
    remove,
    target: menu?.target,
    copying,
    clearCopy: () => setCopying(null),
    overlay,
  };
}

export function useAnnotations(): Record<string, ExperimentAnnotation> {
  const [items, setItems] = useState(readAnnotations);
  useEffect(() => subscribeAnnotations(() => setItems(readAnnotations())), []);
  return items;
}

export function CopySummaryButton({ build }: { build: () => string }) {
  const [copied, setCopied] = useState(false);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(
    () => () => {
      if (timer.current) clearTimeout(timer.current);
    },
    [],
  );
  const copy = () => {
    const text = build();
    if (!navigator.clipboard?.writeText) return;
    void navigator.clipboard.writeText(text).then(() => {
      setCopied(true);
      if (timer.current) clearTimeout(timer.current);
      timer.current = setTimeout(() => setCopied(false), 1800);
    });
  };
  return (
    <button type="button" className="lab-link" onClick={copy}>
      {copied ? t("已复制") : t("复制摘要")}
    </button>
  );
}

export function ExperimentNoteBar({ id, trailing }: { id: string; trailing?: ReactNode }) {
  const annotations = useAnnotations();
  const entry = annotations[id];
  const starred = Boolean(entry?.starred);
  const [draft, setDraft] = useState(entry?.note ?? "");
  useEffect(() => {
    setDraft(entry?.note ?? "");
  }, [id, entry?.note]);
  const commit = () => {
    if (draft.trim() !== (entry?.note ?? "")) saveAnnotation(id, { note: draft });
  };
  return (
    <div className="lab-annotation-bar">
      <button
        type="button"
        className={`lab-star${starred ? " is-starred" : ""}`}
        aria-pressed={starred}
        aria-label={t(starred ? "取消星标" : "加星标")}
        title={t(starred ? "取消星标" : "加星标")}
        onClick={() => saveAnnotation(id, { starred: !starred })}
      >
        {starred ? "★" : "☆"}
      </button>
      <input
        type="text"
        className="lab-note-input"
        aria-label={t("实验备忘")}
        placeholder={t("添加一句话实验结论…")}
        maxLength={160}
        value={draft}
        onChange={event => setDraft(event.target.value)}
        onBlur={commit}
        onKeyDown={event => {
          if (event.key === "Enter") {
            event.preventDefault();
            commit();
            (event.target as HTMLInputElement).blur();
          }
        }}
      />
      {trailing}
    </div>
  );
}
