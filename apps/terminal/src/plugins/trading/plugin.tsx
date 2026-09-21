import {widgets} from "./widgets";
import {AccountPanel} from "./Account";
import type {TerminalPlugin} from "../../extensions/plugins";
import type {PanelContext} from "../workflow/context";
import type {SettingsContext} from "../workflow/settingsContext";
export const plugin:TerminalPlugin<PanelContext,SettingsContext>={
 apiVersion:1,extensions:widgets,id:"asterion.trading",requires:["asterion.overview","asterion.connections"],
 workspaces:[{id:"workspace.trading",title:"交易",icon:"⇄",sections:[{title:"账户与持仓",panel:"trading.account"}]}],
 panels:[{id:"trading.account",scope:"trading",render:c=><><h2>账户与持仓</h2><p className="dashboard-caption">只读查询 · 交易执行未开放</p><AccountPanel api={c.api}/></>}],
};
