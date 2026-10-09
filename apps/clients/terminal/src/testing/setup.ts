import { cleanup } from "@testing-library/react";
import { afterEach, vi } from "vitest";
import { setLocale } from "../i18n";

// Each test starts from an empty document, the default language, the real
// globals and the real clock, whatever the previous one rendered, saved or
// replaced.
afterEach(() => {
  cleanup();
  vi.unstubAllGlobals();
  vi.useRealTimers();
  setLocale("zh-CN");
  window.localStorage.clear();
});
