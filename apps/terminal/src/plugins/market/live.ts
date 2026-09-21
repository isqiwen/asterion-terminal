import type {RequestGrant} from "../../api/requests";
import type {components} from "../../api/schema";
export type MarketState=Omit<components["schemas"]["MarketState"],"configuration"> & {configuration:{subscriptions:components["schemas"]["Subscription"][]}};
export const marketRequests:readonly RequestGrant[]=[{path:"/market",descendants:true,methods:["GET","POST"]},{path:"/connections",descendants:false,methods:["GET"]},{path:"/connections/:id/select",descendants:false,methods:["POST"]},{path:"/connections/:id/connect",descendants:false,methods:["POST"]},{path:"/connections/:id/disconnect",descendants:false,methods:["POST"]}];
