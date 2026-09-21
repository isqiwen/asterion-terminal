import {dashboardContribution} from "../overview/public";
import {AccountPanel} from "./Account";
export {positionColumns} from "./Account";
export const widgets=[
 dashboardContribution({id:"trading.portfolio",title:"账户与持仓",description:"当前连接的只读账户资金与持仓",category:"账户与风险",width:2,defaultVisible:true,column:"secondary",render:c=><AccountPanel api={c.tradingApi}/>}),
 dashboardContribution({id:"trading.risk-metrics",title:"风险概览",description:"资金占用与风险数据完整性",category:"账户与风险",width:2,defaultVisible:true,column:"secondary",render:c=><AccountPanel api={c.tradingApi} risk/>}),
];
