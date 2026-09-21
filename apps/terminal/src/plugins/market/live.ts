import type { RequestGrant } from "../../api/requests";
import type { components } from "../../api/schema";
export type MarketState = Omit<components["schemas"]["MarketState"], "configuration"> & {configuration: MarketConfiguration};
export type MarketConfiguration = Required<components["schemas"]["MarketConfiguration"]>;
export const marketRequests: readonly RequestGrant[] = [{path:"/market",descendants:true,methods:["GET","POST"]}];
