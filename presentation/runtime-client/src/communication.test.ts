import { expect, test, vi } from "vitest";
import cases from "../../../contracts/communication-cases.json";
import { validateWire, communicationHeaders } from "./communication";
import { followEvents, validatePage } from "./events";

for (const fixture of cases)
  test(`shared communication: ${fixture.name}`, () => {
    if (fixture.valid)
      expect(() => validateWire(fixture.type, fixture.value)).not.toThrow();
    else expect(() => validateWire(fixture.type, fixture.value)).toThrow();
  });
test("child call cannot extend its parent deadline", () => {
  const parent = JSON.parse(communicationHeaders(1000)["X-Asterion-Context"]);
  const child = JSON.parse(
    communicationHeaders(10000, parent)["X-Asterion-Context"],
  );
  expect(child.correlation_id).toBe(parent.correlation_id);
  expect(child.causation_id).toBe(parent.request_id);
  expect(child.deadline_ms).toBe(parent.deadline_ms);
});
test("failed consumer replays the page and successful consumer advances", async () => {
  vi.useFakeTimers();
  const seen: string[] = [];
  let fail = true;
  const stop = followEvents(
    async (topic, cursor) => {
      seen.push(cursor);
      return cursor === "latest"
        ? { items: [], cursor: "0" }
        : {
            items: [
              {
                version: 1,
                id: "a".repeat(32),
                topic,
                owner: "fixture",
                stream: "one",
                sequence: "1",
                correlation_id: "b".repeat(32),
                causation_id: null,
                payload: {},
              },
            ],
            cursor: "1",
          };
    },
    ["fixture.changed"],
    async () => {
      if (seen.length > 1 && fail) {
        fail = false;
        throw Error("consumer failed");
      }
    },
    () => {},
  );
  await vi.advanceTimersByTimeAsync(2100);
  expect(seen).toEqual(["latest", "0", "0"]);
  stop();
  await vi.advanceTimersByTimeAsync(2000);
  expect(seen.length).toBe(3);
  vi.useRealTimers();
});
test("cursor must never skip unpublished messages", () => {
  expect(() =>
    validatePage({ items: [], cursor: "20" }, "fixture.changed", "0"),
  ).toThrow();
});

test("a recovered quiet event stream clears its transport error", async () => {
  vi.useFakeTimers();
  const statuses: unknown[] = [];
  let calls = 0;
  const stop = followEvents(async (_, cursor) => {
    calls++;
    if (calls === 2) throw Error("offline");
    return {items:[],cursor:cursor === "latest" ? "0" : cursor};
  }, ["fixture.changed"], async () => {}, e => statuses.push(e));
  await vi.advanceTimersByTimeAsync(2100);
  expect(statuses[1]).toBeInstanceOf(Error);
  expect(statuses.at(-1)).toBeNull();
  stop();
  vi.useRealTimers();
});
