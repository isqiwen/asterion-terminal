import type { UiModule } from "@asterion/workbench/extensions/modules";
import type {PanelContext} from "./context";
import type {SettingsContext} from "./context";
import {ConnectionSettings} from "./ConnectionSettings";
export const uiModule = { id:"asterion.ui.connection-settings",settings:[{id:"connections.settings",scope:"connections",title:"连接",order:3,render:c=><ConnectionSettings api={c.api}/>} ]} satisfies UiModule<PanelContext,SettingsContext>;
