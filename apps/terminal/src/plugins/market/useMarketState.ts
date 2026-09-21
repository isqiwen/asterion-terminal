import { useEffect, useState } from "react";
import type { RequestClient } from "../../api/requests";
import type { MarketState } from "./live";
export function useMarketState(api: RequestClient, connectionId:string) {
  const [state,setState]=useState<MarketState|null>(null);
  const [error,setError]=useState("");
  useEffect(()=>{let disposed=false;let timer:ReturnType<typeof setTimeout>;setState(null);setError("");
    const poll=async()=>{if(!connectionId)return;try{const next=await api.request<MarketState>(`/market/state?connection_id=${connectionId}`);if(!next||next.connection_id!==connectionId||!next.configuration||!Array.isArray(next.configuration.subscriptions)||!Array.isArray(next.quotes)||!next.subscription_errors)throw new Error("行情状态不符合当前接口契约");if(!disposed){setState(next);setError("");}}catch(e){if(!disposed)setError(`行情状态读取失败：${String(e)}`);}finally{if(!disposed)timer=setTimeout(poll,1000);}};
    void poll();return()=>{disposed=true;clearTimeout(timer);};
  },[api,connectionId]);
  return {state,setState,error};
}
