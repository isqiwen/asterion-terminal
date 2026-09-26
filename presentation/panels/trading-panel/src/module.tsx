import {widgets} from "./widgets";
import {AccountPanel} from "./Account";
import type { UiModule } from "@asterion/workbench/extensions/modules";
import type {PanelContext} from "./context";
import type {SettingsContext} from "./context";
export const uiModule = { extensions:widgets,id:"asterion.ui.trading-panel",workspaces:[{id:"workspace.trading",title:"交易",icon:"⇄",sections:[{title:"账户与持仓",panel:"trading.account"}]}],
 panels:[{id:"trading.account",scope:"trading",render:c=><><h2>账户与持仓</h2><p className="dashboard-caption">只读查询 · 交易执行未开放</p><AccountPanel api={c.api}/></>}],
} satisfies UiModule<PanelContext,SettingsContext>;
