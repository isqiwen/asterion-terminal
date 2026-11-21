import { cleanup } from "@testing-library/react";
import { afterEach, vi } from "vitest";
import { setLocale } from "../i18n";

// jsdom has no layout or modal dialog support.
Element.prototype.scrollIntoView = () => {};
Range.prototype.getClientRects = () => [] as unknown as DOMRectList;
if (!HTMLDialogElement.prototype.showModal) {
  HTMLDialogElement.prototype.showModal = function () {
    this.setAttribute("open", "");
  };
}
if (!HTMLDialogElement.prototype.close) {
  HTMLDialogElement.prototype.close = function () {
    this.removeAttribute("open");
  };
}

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
