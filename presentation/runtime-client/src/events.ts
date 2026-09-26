import type { EventPage } from "@asterion/api-types/wire.generated";
export type { EventPage } from "@asterion/api-types/wire.generated";
import { validateWire } from "./communication";

export function validatePage(value: EventPage, topic: string, after?: string) {
  validateWire("EventPage", value);
  if (
    !value ||
    !Array.isArray(value.items) ||
    typeof value.cursor !== "string" ||
    !/^(0|[1-9][0-9]*)$/.test(value.cursor)
  )
    throw new Error("事件分页不符合通信契约");
  let previous = BigInt(after ?? "0");
  for (const event of value.items) {
    validateWire("Event", event);
    if (
      !/^[1-9][0-9]*$/.test(event.sequence) ||
      event.topic !== topic ||
      BigInt(event.sequence) <= previous
    )
      throw new Error("事件顺序或主题不一致");
    previous = BigInt(event.sequence);
  }
  if (after !== undefined && BigInt(value.cursor) !== previous)
    throw new Error("事件游标不一致");
}

/** Bounded replay. Advance only after the consumer succeeds; duplicates are expected. */
export function followEvents(
  read: (topic: string, cursor: string) => Promise<EventPage>,
  topics: readonly string[],
  refresh: () => Promise<void>,
  error: (reason: unknown) => void,
) {
  let live = true;
  let ready = false;
  let timer: ReturnType<typeof setTimeout>;
  const cursors = new Map<string, string>();
  const tick = async () => {
    try {
      if (!ready) {
        for (const topic of topics) {
          const page = await read(topic, "latest");
          if (!live) return;
          validatePage(page, topic);
          cursors.set(topic, page.cursor);
        }
        await refresh();
        ready = true;
      } else {
        for (const topic of topics) {
          const cursor = cursors.get(topic)!;
          const page = await read(topic, cursor);
          if (!live) return;
          validatePage(page, topic, cursor);
          if (page.items.length) await refresh();
          if (!live) return;
          cursors.set(topic, page.cursor);
        }
      }
      if (live) error(null);
    } catch (e) {
      if (live) error(e);
    } finally {
      if (live) timer = setTimeout(tick, 1000);
    }
  };
  void tick();
  return () => {
    live = false;
    clearTimeout(timer);
  };
}
