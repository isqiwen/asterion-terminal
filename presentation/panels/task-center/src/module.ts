import { widgets } from "./widgets";
import type { UiModule } from "@asterion/workbench/extensions/modules";
export const uiModule = { 
  extensions: widgets,
  id: "asterion.ui.task-center",
  } satisfies UiModule<{}, {}>;
