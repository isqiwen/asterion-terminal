import { fireEvent } from "@testing-library/react";

// jsdom has no invoker commands; browser tests exercise the native click and dismissal.
export function openPopover(trigger: HTMLElement) {
  const panel = document.getElementById(trigger.getAttribute("commandfor")!)!;
  panel.style.display = "block";
  fireEvent(panel, Object.assign(new Event("toggle"), { newState: "open" }));
}
