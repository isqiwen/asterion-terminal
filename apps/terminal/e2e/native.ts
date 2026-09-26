import { parseLayout } from "@asterion/ui-terminal-workspace/layout";
import type { BrowserContext, Page } from "@playwright/test";
export async function nativeContext(
  context: BrowserContext,
  handlers: Record<
    string,
    (args: Record<string, unknown>) => unknown | Promise<unknown>
  > = {},
) {
  // Synthetic core event source for UI fixtures. Each page signals a refresh;
  // dedicated communication tests verify real cursor and retry semantics.
  const events = async (page: Page) => page.route("**/api/v1/communication/events**", route => {
    const url = new URL(route.request().url());
    const after = url.searchParams.get("after") ?? "0";
    const topic = url.searchParams.get("topic")!;
    if (after === "latest") return route.fulfill({json: {items: [], cursor: "0"}});
    const sequence = String(BigInt(after) + 1n);
    return route.fulfill({json: {items: [{version:1,id:"a".repeat(32),topic,owner:"fixture",stream:"fixture",sequence,correlation_id:"b".repeat(32),causation_id:null,payload:{}}],cursor:sequence}});
  });
  await Promise.all(context.pages().map(events));
  context.on("page", page => { void events(page); });
  let elapsed = 0,
    pin = "246810";
  let security = {
    locked: false,
    timeout_seconds: 300,
    remaining_seconds: 300,
    revision: 0,
    retry_after: 0,
  };
  let activity = Date.now();
  await context.route("**/api/v1/account/security**", (route) => {
    const action = new URL(route.request().url()).pathname.split(
      "/security",
    )[1];
    const now = Date.now() + elapsed;
    if (now - activity >= security.timeout_seconds * 1000 && !security.locked) {
      security.locked = true;
      security.revision++;
    }
    const body = route.request().postDataJSON() ?? {};
    if (action === "/lock") {
      security.locked = true;
      security.revision++;
    }
    if (action === "/activity" && !security.locked) activity = now;
    if (action === "/unlock") {
      if (body.pin !== pin)
        return route.fulfill({
          status: 401,
          json: { detail: "PIN 不正确", code: "INVALID_PIN" },
        });
      security.locked = false;
      security.revision++;
      activity = now;
    }
    if (action === "/change") {
      pin = body.pin;
      security.locked = false;
      security.revision++;
      activity = now;
    }
    if (action === "/timeout") {
      security.timeout_seconds = body.seconds;
      activity = now;
    }
    return route.fulfill({
      json: {
        ...security,
        remaining_seconds: Math.max(
          0,
          security.timeout_seconds - (now - activity) / 1000,
        ),
      },
    });
  });
  let revision = 0;
  let workspaceRevision = 0;
  const layouts = new Map<string, Record<string, unknown>>();
  let token: string | null = null;
  await context.exposeBinding(
    "nativeInvoke",
    async (
      _source,
      command: string,
      args: {
        expected: number;
        token: string | null;
        patch?: Record<string, unknown>;
        id?: string;
        kind?: string;
      } = {
        expected: 0,
        token: null,
      },
    ) => {
      if (handlers[command]) return handlers[command](args);
      if (command === "desktop_setup_status")
        return {
          ready: true,
          running: false,
          step: 5,
          downloaded: 0,
          network: null,
          dependencies: { total: null, installed: 0, phase: "", current: "" },
          total: null,
          error: "",
          directory: "/test/runtime",
        };
      if (command === "desktop_setup_window") return;
      const url = new URL(_source.page.url());
      const id = url.searchParams.get("window") ?? "main";
      if (!layouts.has(id))
        layouts.set(id, {
          ...parseLayout(null),
          view: url.searchParams.get("view") ?? "市场",
        });
      if (command === "desktop_workspace_read")
        return {
          id,
          revision: workspaceRevision,
          layout: {
            ...layouts.get(id),
            detached: [...layouts.values()].some(
              (r) => r.owner === id && r.ready && r.open,
            ),
          },
        };
      if (command === "desktop_workspace_patch") {
        if (args.expected !== workspaceRevision)
          throw new Error("WORKSPACE_CONFLICT");
        Object.assign(layouts.get(id)!, args.patch);
        return { id, revision: ++workspaceRevision, layout: layouts.get(id) };
      }
      if (command === "desktop_workspace_ready") {
        Object.assign(layouts.get(id)!, { ready: true, open: true });
        return;
      }
      if (command === "desktop_workspace_open") {
        layouts.set(args.id!, {
          ...layouts.get(id),
          kind: args.kind,
          owner: args.kind === "chart" ? id : "",
          ready: false,
          open: true,
          section: "历史图表",
        });
        const child = await context.newPage();
        child.on("close", () =>
          Object.assign(layouts.get(args.id!)!, { open: false, ready: false }),
        );
        await child.goto(`/?window=${args.id}`);
        return;
      }
      if (command === "desktop_workspace_merge") {
        const child =
          layouts.get(id)!.kind === "chart"
            ? id
            : [...layouts].find(([, r]) => r.owner === id && r.open)?.[0];
        if (child) {
          const row = layouts.get(child)!;
          if (typeof row.owner === "string" && layouts.has(row.owner))
            Object.assign(layouts.get(row.owner)!, {
              snapshot: row.snapshot,
              contract: row.contract,
              viewport: row.viewport,
            });
          Object.assign(row, { open: false, ready: false });
          const childPage = context
            .pages()
            .find((p) => new URL(p.url()).searchParams.get("window") === child);
          if (childPage) await childPage.close();
        }
        return;
      }
      if (command === "open_settings") {
        const existing = context
          .pages()
          .find(
            (p) => new URL(p.url()).searchParams.get("screen") === "settings",
          );
        if (existing) await existing.bringToFront();
        else {
          const settingsPage = await context.newPage();
          await settingsPage.goto(
            "http://127.0.0.1:1420/?screen=settings&window=settings",
          );
        }
        return;
      }
      if (command === "desktop_account_read") return { revision, token };
      if (command === "desktop_account_write") {
        if (args.expected !== revision) throw new Error("stale revision");
        token = args.token;
        revision++;
        return;
      }
      if (command === "desktop_window_id")
        return new URL(_source.page.url()).searchParams.get("window") ?? "main";
      if (command === "plugin:window|is_decorated") return true;
      if (command === "plugin:window|set_title") return;
      return {
        api_url: "http://127.0.0.1:8000",
        token: "fixture",
        data_directory: "/test",
      };
    },
  );
  await context.addInitScript(() => {
    const host = window as unknown as {
      nativeInvoke: (...args: unknown[]) => Promise<unknown>;
    };
    Object.assign(window, {
      isTauri: true,
      __TAURI_INTERNALS__: {
        metadata: {
          currentWindow: {
            label: new URLSearchParams(location.search).get("window") ?? "main",
          },
        },
        invoke: (...args: unknown[]) => host.nativeInvoke(...args),
      },
    });
  });
  return {
    layout: (id = "main") => structuredClone(layouts.get(id)),
    advance: (seconds: number) => {
      elapsed += seconds * 1000;
    },
  };
}
