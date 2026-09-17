use serde_json::{json, Value};
use std::{collections::BTreeMap, fs, path::PathBuf, sync::Mutex};
use tauri::{AppHandle, Manager, WebviewUrl, WebviewWindow, WebviewWindowBuilder};

#[derive(Default)]
pub struct Workspace(pub Mutex<Store>);
#[derive(Default)]
pub struct Store {
    pub revision: u64,
    pub windows: BTreeMap<String, Value>,
    pub path: PathBuf,
    pub quitting: bool,
}
fn default_record() -> Value { json!({"version":4,"view":"市场","inspector":false,"tasks":false,"taskHeight":180,"marketRatio":60,"snapshot":"","contract":"","section":"行情全景","linkGroup":"A","locked":false,"viewport":null,"dock":"left","kind":"workspace","owner":"","ready":false,"open":true}) }
impl Store {
    fn save(&self) -> Result<(), String> {
        if self.path.as_os_str().is_empty() { return Ok(()); }
        let tmp = self.path.with_extension("tmp");
        fs::write(&tmp, serde_json::to_vec(&json!({"version":1,"revision":self.revision,"windows":self.windows})).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
        fs::rename(tmp, &self.path).map_err(|e| e.to_string())
    }
    pub fn read(&self, id: &str) -> Value {
        let mut record = self.windows.get(id).cloned().unwrap_or_else(default_record);
        record["detached"] = json!(self.windows.values().any(|v| v["owner"] == id && v["ready"] == true && v["open"] == true));
        json!({"revision":self.revision,"id":id,"layout":record})
    }
    fn patch(&mut self, id: &str, expected: u64, patch: &Value) -> Result<(), String> {
        if expected != self.revision { return Err("WORKSPACE_CONFLICT".into()); }
        let values = patch.as_object().ok_or("Invalid workspace patch")?;
        let allowed = ["version","view","inspector","tasks","taskHeight","marketRatio","snapshot","contract","section","linkGroup","locked","viewport","dock"];
        if values.keys().any(|k| !allowed.contains(&k.as_str())) { return Err("Unknown workspace field".into()); }
        for (key, value) in values {
            let valid = match key.as_str() {
                "version" => value == 4,
                "dock" => value.as_str().is_some_and(|v| ["left","right","top","bottom"].contains(&v)),
                "viewport" => value.is_null() || (value["snapshot"].as_str().is_some_and(|v|v.len()<=512) && value["contract"].as_str().is_some_and(|v|v.len()<=512) && value["from"].as_f64().zip(value["to"].as_f64()).is_some_and(|(a,b)| a.is_finite() && b.is_finite() && a<b && a.abs()<1e9 && b.abs()<1e9)),
                "inspector" | "tasks" | "locked" => value.is_boolean(),
                "marketRatio" => value.as_f64().is_some_and(|n| (30.0..=75.0).contains(&n)),
                "taskHeight" => value.as_f64().is_some_and(|n| (120.0..=400.0).contains(&n)),
                "view" => value.as_str().is_some_and(|v| ["总览","市场","数据","研究","交易","情报"].contains(&v)),
                "linkGroup" => value.as_str().is_some_and(|v| ["","A","B","C"].contains(&v)),
                _ => value.as_str().is_some_and(|v| v.len() <= 512),
            };
            if !valid { return Err(format!("Invalid workspace field: {key}")); }
        }
        let record = self.windows.entry(id.into()).or_insert_with(default_record);
        for (key, value) in values { record[key] = value.clone(); }
        let group = record["linkGroup"].as_str().unwrap_or("").to_owned();
        let snapshot = record["snapshot"].clone();
        let contract = record["contract"].clone();
        if !group.is_empty() && (values.contains_key("snapshot") || values.contains_key("contract")) {
            for (other, row) in self.windows.iter_mut() {
                if other != id && row["linkGroup"] == group && row["locked"] != true {
                    row["snapshot"] = snapshot.clone(); row["contract"] = contract.clone();
                }
            }
        }
        self.revision += 1;
        self.save()
    }
    fn close(&mut self, id: &str) {
        if let Some(record) = self.windows.get_mut(id) { record["open"] = json!(false); record["ready"] = json!(false); }
        self.revision += 1;
    }
}
fn launch(app: &AppHandle, id: &str, record: &Value) -> Result<(), String> {

    let title = if record["kind"] == "chart" { "历史图表 · Asterion" } else { "工作台 · Asterion" };
    let win = if let Some(existing) = app.get_webview_window(id) { existing } else { WebviewWindowBuilder::new(app, id, WebviewUrl::App("index.html".into())).title(title).inner_size(1100.0, 760.0).min_inner_size(800.0, 600.0).center().build().map_err(|e| e.to_string())? };
    if let Some(rect) = record["placement"].as_object() {
        let monitors = win.available_monitors().map_err(|e|e.to_string())?;
        let preferred = rect.get("monitor").and_then(Value::as_str);
        let monitor = monitors.iter().find(|m|m.name().map(String::as_str)==preferred).or_else(||monitors.first());
        if let Some(monitor) = monitor {
            let scale = monitor.scale_factor();
            let area = monitor.work_area();
            let number = |key:&str, fallback:f64| rect.get(key).and_then(Value::as_f64).filter(|n|n.is_finite()).unwrap_or(fallback);
            let w = (number("width",1100.0).clamp(800.0,5000.0)*scale).min(area.size.width as f64) as u32;
            let h = (number("height",760.0).clamp(600.0,4000.0)*scale).min(area.size.height as f64) as u32;
            let x = area.position.x + (number("x",0.0)*scale).clamp(0.0,(area.size.width-w) as f64) as i32;
            let y = area.position.y + (number("y",0.0)*scale).clamp(0.0,(area.size.height-h) as f64) as i32;
            win.set_size(tauri::PhysicalSize::new(w,h)).map_err(|e|e.to_string())?;
            win.set_position(tauri::PhysicalPosition::new(x,y)).map_err(|e|e.to_string())?;
        }
    }
    Ok(())
}
pub fn reopen_main(app: &AppHandle) -> Result<(), String> {
    let row = {
        let state=app.state::<Workspace>();
        let mut store=state.0.lock().map_err(|e|e.to_string())?;
        store.quitting=false;
        store.windows.get("main").cloned().unwrap_or_else(default_record)
    };
    launch(app,"main",&row)?;
    if let Some(window)=app.get_webview_window("main") {
        window.unminimize().map_err(|e|e.to_string())?;
        window.show().map_err(|e|e.to_string())?;
        window.set_focus().map_err(|e|e.to_string())?;
    }
    Ok(())
}
pub fn initialize(app: &AppHandle) -> Result<(), String> {
    let dir = app.path().app_data_dir().map_err(|e| e.to_string())?;
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let state = app.state::<Workspace>();
    let restore;
    {
        let mut s=state.0.lock().map_err(|e| e.to_string())?;
        s.path=dir.join("workspace.json");
        if s.path.exists() {
            let bytes = fs::read(&s.path).map_err(|e| e.to_string())?;
            let value: Value = match serde_json::from_slice(&bytes) {
                Ok(value) => value,
                Err(_) => { fs::write(s.path.with_extension("invalid.json"), &bytes).map_err(|e|e.to_string())?; json!({}) }
            };
            s.revision=value["revision"].as_u64().unwrap_or(0);
            if let Some(rows)=value["windows"].as_object() {
                for (id, row) in rows { if (id=="main" || id.starts_with("workspace-")) && row.is_object() { let mut row=row.clone(); row["ready"]=json!(false); s.windows.insert(id.clone(),row); } }
            }
        }
        s.windows.entry("main".into()).or_insert_with(default_record);
        restore=s.windows.iter().filter(|(id,v)| id.as_str()=="main" || v["open"]==true).map(|(id,v)|(id.clone(),v.clone())).collect::<Vec<_>>();
    }
    for (id,row) in restore { if let Err(e)=launch(app,&id,&row) { eprintln!("Cannot restore workspace: {e}"); } }
    Ok(())
}
#[tauri::command]
pub fn desktop_workspace_read(window: WebviewWindow, state: tauri::State<Workspace>) -> Result<Value,String> {
    Ok(state.0.lock().map_err(|e|e.to_string())?.read(window.label()))
}
#[tauri::command]
pub fn desktop_workspace_patch(window: WebviewWindow, state: tauri::State<Workspace>, expected: u64, patch: Value) -> Result<Value,String> {
    let mut s=state.0.lock().map_err(|e|e.to_string())?;
    s.patch(window.label(),expected,&patch)?;
    Ok(s.read(window.label()))
}
#[tauri::command]
pub fn desktop_workspace_open(app: AppHandle, window: WebviewWindow, state: tauri::State<Workspace>, id: String, kind: String) -> Result<(),String> {
    if !id.starts_with("workspace-") || id.len()>80 || !id.chars().all(|c| c.is_ascii_alphanumeric() || c=='-') || !["chart","workspace"].contains(&kind.as_str()) { return Err("Invalid window".into()); }
    let row;
    {
        let mut s=state.0.lock().map_err(|e|e.to_string())?;
        if s.windows.contains_key(&id) { return Err("Window already registered".into()); }
        if kind=="chart" && s.windows.values().any(|r| r["owner"]==window.label() && r["open"]==true) { return Err("图表窗口已打开".into()); }
        let mut r=s.windows.get(window.label()).cloned().unwrap_or_else(default_record);
        r["kind"]=json!(kind); r["owner"]=json!(if kind=="chart" {window.label()} else {""}); r["ready"]=json!(false); r["open"]=json!(true); r.as_object_mut().unwrap().remove("placement");
        if kind=="chart" { r["view"]=json!("市场"); r["section"]=json!("历史图表"); }
        s.windows.insert(id.clone(),r.clone()); s.revision+=1; row=r;
    }
    if let Err(e)=launch(&app,&id,&row) { let mut s=state.0.lock().map_err(|e|e.to_string())?; s.windows.remove(&id); s.revision+=1; return Err(e); }
    Ok(())
}
#[tauri::command]
pub fn desktop_workspace_ready(window: WebviewWindow, state: tauri::State<Workspace>) -> Result<(),String> {
    let mut s=state.0.lock().map_err(|e|e.to_string())?;
    let r=s.windows.entry(window.label().into()).or_insert_with(default_record);
    if r["ready"]!=true { r["ready"]=json!(true); r["open"]=json!(true); s.revision+=1; s.save()?; }
    Ok(())
}
#[tauri::command]
pub fn desktop_workspace_merge(app: AppHandle, window: WebviewWindow, state: tauri::State<Workspace>) -> Result<(),String> {
    let child;
    {
        let mut s=state.0.lock().map_err(|e|e.to_string())?;
        let own=s.windows.get(window.label()).cloned().ok_or("Window missing")?;
        child=if own["kind"]=="chart" { window.label().to_owned() } else { s.windows.iter().find(|(_,r)|r["owner"]==window.label() && r["open"]==true).map(|(id,_)|id.clone()).ok_or("没有拆出的图表")? };
        let r=s.windows.get(&child).cloned().ok_or("Panel missing")?;
        let owner=r["owner"].as_str().ok_or("Owner missing")?;
        if app.get_webview_window(owner).is_none() { return Err("原窗口已关闭，请保留当前图表窗口".into()); }
        if let Some(parent)=s.windows.get_mut(owner) { parent["snapshot"]=r["snapshot"].clone(); parent["contract"]=r["contract"].clone(); parent["viewport"]=r["viewport"].clone(); }
        s.close(&child); s.save()?;
    }
    if let Some(win)=app.get_webview_window(&child) { win.close().map_err(|e|e.to_string())?; }
    Ok(())
}
pub fn window_event(window: &tauri::Window, event: &tauri::WindowEvent) {
    if window.label()=="settings" { return; }
    let app=window.app_handle(); let state=app.state::<Workspace>();
    if let Ok(mut s)=state.0.lock() {
        match event {
            tauri::WindowEvent::Destroyed if !s.quitting => { s.close(window.label()); let _=s.save(); },
            tauri::WindowEvent::Moved(_) | tauri::WindowEvent::Resized(_) => {
                if let (Ok(pos),Ok(size))=(window.outer_position(),window.inner_size()) {
                    if let (Some(r),Ok(Some(monitor)))=(s.windows.get_mut(window.label()),window.current_monitor()) {
                        let scale=monitor.scale_factor(); let area=monitor.work_area();
                        r["placement"]=json!({"monitor":monitor.name(),"x":(pos.x-area.position.x) as f64/scale,"y":(pos.y-area.position.y) as f64/scale,"width":size.width as f64/scale,"height":size.height as f64/scale});
                        let _=s.save();
                    }
                }
            }, _=>{}
        }
    };
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn compare_revision_and_respect_link_locks() {
        let mut s=Store::default(); s.windows.insert("main".into(),default_record()); s.windows.insert("peer".into(),default_record());
        s.patch("main",0,&json!({"contract":"SHFE.rb2610","snapshot":"s1"})).unwrap(); assert_eq!(s.windows["peer"]["contract"],"SHFE.rb2610");
        assert!(s.patch("peer",0,&json!({"view":"数据"})).is_err());
        s.patch("peer",1,&json!({"locked":true})).unwrap(); s.patch("main",2,&json!({"contract":"SHFE.rb2611"})).unwrap(); assert_eq!(s.windows["peer"]["contract"],"SHFE.rb2610");
    }
    #[test] fn detach_requires_ready_and_close_restores_owner() {
        let mut s=Store::default(); s.windows.insert("main".into(),default_record()); let mut child=default_record(); child["owner"]=json!("main"); s.windows.insert("chart".into(),child);
        assert_eq!(s.read("main")["layout"]["detached"],false); s.windows.get_mut("chart").unwrap()["ready"]=json!(true); assert_eq!(s.read("main")["layout"]["detached"],true); s.close("chart"); assert_eq!(s.read("main")["layout"]["detached"],false);
    }
    #[test] fn persisted_layout_and_invalid_patch() {
        let dir=std::env::temp_dir().join(format!("asterion-layout-{}",std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let mut s=Store {path:dir.join("workspace.json"),..Default::default()};
        assert!(s.patch("main",0,&json!({"marketRatio":99})).is_err());
        assert_eq!(s.revision,0);
        s.patch("main",0,&json!({"marketRatio":45,"view":"数据"})).unwrap();
        let saved:Value=serde_json::from_slice(&fs::read(&s.path).unwrap()).unwrap();
        assert_eq!(saved["windows"]["main"]["marketRatio"],45);
        assert_eq!(saved["windows"]["main"]["view"],"数据");
        fs::remove_dir_all(dir).unwrap();
    }

    #[test] fn chart_state_validates_and_moves_with_window_record() {
        let mut s=Store::default();
        let viewport=json!({"snapshot":"s","contract":"SHFE.rb2610","from":-5.0,"to":80.0});
        s.patch("main",0,&json!({"viewport":viewport,"dock":"bottom"})).unwrap();
        assert_eq!(s.read("main")["layout"]["viewport"],viewport);
        assert_eq!(s.read("main")["layout"]["dock"],"bottom");
        assert!(s.patch("main",1,&json!({"viewport":{"snapshot":"s","contract":"c","from":10,"to":5}})).is_err());
        assert_eq!(s.revision,1);
        assert_eq!(s.windows["main"]["viewport"],viewport);
    }

}
