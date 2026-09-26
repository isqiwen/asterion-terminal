import type { UiModule } from "@asterion/workbench/extensions/modules";

/** Public rules editor is consumed by the declared research dependency. */
export const uiModule = {
  id: "asterion.ui.market-rules-controls",
} satisfies UiModule<{}, {}>;
