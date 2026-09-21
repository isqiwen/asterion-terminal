import type {TerminalPlugin} from "../../extensions/plugins";
import type {PanelContext} from "../workflow/context";
import type {SettingsContext} from "../workflow/settingsContext";
import {ConnectionSettings} from "./ConnectionSettings";
export const plugin:TerminalPlugin<PanelContext,SettingsContext>={apiVersion:1,id:"asterion.connections",requires:[], settings:[{id:"connections.settings",scope:"connections",title:"连接",order:3,render:c=><ConnectionSettings api={c.api}/>} ]};
