import {useEffect,useId,useRef,useState} from "react";
import type {RequestClient} from "@asterion/runtime-client/requests";
import type {components} from "@asterion/api-types/schema";
import {useConnections,type Connection} from "./public";
import "./connections.css";
type Save=components["schemas"]["SaveConnection"];
type Secret=components["schemas"]["SecretChange"];
export function ConnectionSettings({api}:{api:RequestClient}){
 const {data,error,selected,select}=useConnections(api);
 const [draft,setDraft]=useState<Save|null>(null),[busy,setBusy]=useState(false),[message,setMessage]=useState("");
 const [deleting,setDeleting]=useState<Connection|null>(null);
 const descriptor=data?.connectors.find(c=>c.id===draft?.connector_id);
 const active=!!selected&&data?.active_id===selected.connection_id;
 function create(connectorId:string){const d=data?.connectors.find(c=>c.id===connectorId);if(!d)return;setDraft({connector_id:d.id,name:"",config:Object.fromEntries(d.fields.filter(f=>!f.secret).map(f=>[f.key,f.default??""])),secrets:Object.fromEntries(d.fields.filter(f=>f.secret).map(f=>[f.key,{action:"replace",value:""}]))});setMessage("");}
 async function perform(action:()=>Promise<unknown>){setBusy(true);setMessage("");try{await action();}catch(e){setMessage(String(e));}finally{setBusy(false);}}
 async function save(){if(!draft)return;await perform(async()=>{await api.request("/connections",draft);setDraft(null);setMessage("配置已保存，可从列表选择使用。");});}
 function edit(){if(!selected)return;const d=data?.connectors.find(c=>c.id===selected.connector_id);if(!d)return;setDraft({connection_id:selected.connection_id,expected_revision:selected.config_revision,connector_id:selected.connector_id,name:selected.name,config:{...selected.config},secrets:Object.fromEntries(d.fields.filter(f=>f.secret).map(f=>[f.key,selected.secret_saved[f.key]?{action:"keep"}:{action:"replace",value:""}]))});setMessage("");}
 return <section aria-label="账户连接"><h2>账户连接</h2><p>选择已保存的配置即可使用；切换时自动断开原连接。密码加密保存在本机。</p>
 {(error||data?.notice)&&<p role="alert">{error||data?.notice}</p>}
 <div className="dashboard-controls"><select aria-label="当前连接" disabled={busy||!!draft||!!deleting} value={selected?.connection_id??""} onChange={e=>void perform(()=>select(e.target.value))}><option value="" disabled>选择连接</option>{data?.connections.map(c=><option key={c.connection_id} value={c.connection_id}>{c.name}</option>)}</select><button disabled={busy||!!draft||!data?.connectors.length} onClick={()=>create(data!.connectors[0].id)}>添加配置</button></div>
 {selected&&!draft&&<><p>{!selected.available?"接入插件不可用":active?`${selected.market.detail} · ${selected.account.detail}`:"未连接"}</p><div className="dashboard-controls"><button disabled={busy||!selected.available} onClick={()=>void perform(()=>api.request(`/connections/${selected.connection_id}/${active?"disconnect":"connect"}`,{}))}>{busy?"处理中…":active?"断开连接":"连接"}</button><button disabled={busy||active||!selected.available} onClick={edit}>编辑配置</button><button disabled={busy||active} onClick={()=>setDeleting(selected)}>删除配置</button></div>{active&&<p>编辑或删除前请先断开连接。</p>}</>}
 {draft&&<form aria-label="连接配置" onSubmit={e=>{e.preventDefault();void save();}}><fieldset disabled={busy} className="live-configuration">
 {data&&data.connectors.length>1&&<label>接入协议<select value={draft.connector_id} disabled={!!draft.connection_id} onChange={e=>create(e.target.value)}>{data.connectors.map(c=><option key={c.id} value={c.id}>{c.title}</option>)}</select></label>}
 <label>连接名称<input required maxLength={60} value={draft.name} onChange={e=>setDraft({...draft,name:e.target.value})}/></label>
 {descriptor?.fields.map(f=>f.secret?<SecretInput key={f.key} label={f.label} required={f.required} saved={!!data?.connections.find(c=>c.connection_id===draft.connection_id)?.secret_saved[f.key]} change={draft.secrets[f.key]} onChange={change=>setDraft({...draft,secrets:{...draft.secrets,[f.key]:change}})}/>:<label key={f.key}>{f.label}<input required={f.required} disabled={!!draft.connection_id&&f.identity} value={draft.config[f.key]??""} onChange={e=>setDraft({...draft,config:{...draft.config,[f.key]:e.target.value}})}/></label>)}
 {draft.connection_id&&descriptor?.fields.some(f=>f.identity)&&<p>账户标识固定；更换账户请添加配置。</p>}
 {descriptor?.instructions&&<details><summary>参数说明</summary><p>{descriptor.instructions}</p></details>}
 <button type="submit">保存配置</button><button type="button" onClick={()=>setDraft(null)}>取消</button></fieldset></form>}
 {deleting&&<DeleteProfile api={api} connection={deleting} close={()=>setDeleting(null)} removed={()=>{setDeleting(null);setMessage("配置及保存的凭据已删除，历史数据已保留。");}}/>}
 {message&&<p role="status">{message}</p>}
 </section>;
}

function SecretInput({label,required,saved,change,onChange}:{label:string;required:boolean;saved:boolean;change:Secret;onChange:(change:Secret)=>void}){
 const id=useId();const input=useRef<HTMLInputElement>(null);
 const editing=change.action==="replace";
 useEffect(()=>{if(editing&&saved)input.current?.focus();},[editing,saved]);
 return <div className="connection-secret"><label htmlFor={id}>{label}</label><div className="connection-secret-control"><input ref={input} id={id} type="password" autoComplete="new-password" readOnly={!editing} required={required&&editing} placeholder={editing?`请输入${label}`:"••••••••（已保存）"} value={editing?change.value??"":""} onChange={e=>onChange({action:"replace",value:e.target.value})}/>{saved&&<button type="button" aria-label={`${editing?"取消修改":"修改"}${label}`} onClick={()=>onChange(editing?{action:"keep"}:{action:"replace",value:""})}>{editing?"取消修改":"修改"}</button>}</div></div>;
}

function DeleteProfile({api,connection,close,removed}:{api:RequestClient;connection:Connection;close:()=>void;removed:()=>void}){
 const dialog=useRef<HTMLDialogElement>(null);
 const [busy,setBusy]=useState(false),[error,setError]=useState("");
 useEffect(()=>{const previous=document.activeElement;dialog.current?.showModal();return()=>{dialog.current?.close();if(previous instanceof HTMLElement)previous.focus();};},[]);
 async function remove(){setBusy(true);setError("");try{await api.request(`/connections/${connection.connection_id}/delete`,{expected_revision:connection.config_revision});removed();}catch(e){setError(String(e));}finally{setBusy(false);}}
 return <dialog ref={dialog} className="widget-catalog" aria-label="删除连接配置" onCancel={e=>{e.preventDefault();if(!busy)close();}}><h2>删除「{connection.name}」？</h2><p>将删除此配置及保存的密码。自选缓存和历史数据保留；再次使用需要重新填写连接参数。</p>{error&&<p role="alert">{error}</p>}<div className="dashboard-controls"><button autoFocus disabled={busy} onClick={close}>取消</button><button disabled={busy} onClick={()=>void remove()}>{busy?"删除中…":"确认删除"}</button></div></dialog>;
}
