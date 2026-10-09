import { expect, test } from "vitest";
import { registerLanguageResources, translate } from ".";

test("language resources validate namespaces, keys and interpolation", () => {
  const catalog = { "zh-CN": { hello: "你好 {name}" }, "en-US": { hello: "Hello {name}" } };
  registerLanguageResources("test.languages", catalog);
  expect(translate("test.languages", "hello", { name: "Asterion" })).toBe("你好 Asterion");
  expect(() => translate("test.languages", "hello")).toThrow("argument");
  expect(() => translate("test.languages", "missing")).toThrow("Unknown translation");
  expect(() => registerLanguageResources("test.languages", catalog)).toThrow("Duplicate");
  expect(() => registerLanguageResources("test.keys", { ...catalog, "en-US": {} })).toThrow(
    "keys differ",
  );
  expect(() =>
    registerLanguageResources("test.arguments", {
      ...catalog,
      "en-US": { hello: "Hello {other}" },
    }),
  ).toThrow("placeholders differ");
});
