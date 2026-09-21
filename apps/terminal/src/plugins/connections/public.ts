import {useCallback,useEffect,useState} from "react";
import type {RequestClient,RequestGrant} from "../../api/requests";
import type {components} from "../../api/schema";
export type Connections=components["schemas"]["ConnectionList"];
export type Connection=components["schemas"]["ConnectionView"];
export const connectionRequests:readonly RequestGrant[]=[{path:"/connections",descendants:true,methods:["GET","POST"]}];
const changed="asterion.connections-changed";
export function useConnections(api:RequestClient){
 const [data,setData]=useState<Connections|null>(null),[error,setError]=useState("");
 useEffect(()=>{let disposed=false;let timer:ReturnType<typeof setTimeout>;let revision=0;
  const poll=async()=>{const request=++revision;clearTimeout(timer);try{const next=await api.request<Connections>("/connections");if(!Array.isArray(next.connections)||!Array.isArray(next.connectors))throw new Error("连接列表不符合契约");if(!disposed&&request===revision){setData(next);setError("");}}catch(e){if(!disposed&&request===revision)setError(String(e));}finally{if(!disposed&&request===revision)timer=setTimeout(poll,1000);}};
  const refresh=()=>void poll();window.addEventListener(changed,refresh);void poll();return()=>{disposed=true;clearTimeout(timer);window.removeEventListener(changed,refresh);};},[api]);
 const select=useCallback(async(id:string)=>{await api.request(`/connections/${id}/select`,{});window.dispatchEvent(new Event(changed));},[api]);
 const selected=data?.connections.find(c=>c.connection_id===data.selected_id)??null;
 return {data,error,id:data?.selected_id??"",selected,select};
}
