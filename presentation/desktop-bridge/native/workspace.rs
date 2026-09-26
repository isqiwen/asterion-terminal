use serde_json::{Value, json};
use std::{collections::BTreeMap, fs, path::PathBuf, sync::Mutex};
use tauri::{AppHandle, Manager, WebviewUrl, WebviewWindow, WebviewWindowBuilder};

pub struct WindowProfile {
    pub default_record: fn() -> Value,
    pub validate: fn(&Value) -> Result<(), String>,
    pub linked_fields: &'static [&'static str],
    pub merge_fields: &'static [&'static str],
    pub detached_kind: &'static str,
    pub duplicate_detached_error: &'static str,
    pub missing_detached_error: &'static str,
    pub owner_closed_error: &'static str,
    pub prepare_child: fn(&mut Value),
    pub title: fn(&Value) -> &'static str,
}
pub struct Workspace(Mutex<Store>);
impl Workspace {
    pub fn new(profile: &'static WindowProfile) -> Self {
        Self(Mutex::new(Store::new(profile)))
    }
}
struct Store {
    profile: &'static WindowProfile,
    pub revision: u64,
    pub windows: BTreeMap<String, Value>,
    pub path: PathBuf,
    pub quitting: bool,
    pub load_error: Option<String>,
}
impl Store {
    fn new(profile: &'static WindowProfile) -> Self {
        Self {
            profile,
            revision: 0,
            windows: BTreeMap::new(),
            path: PathBuf::new(),
            quitting: false,
            load_error: None,
        }
    }

    fn save(&self) -> Result<(), String> {
        if let Some(error) = &self.load_error {
            return Err(error.clone());
        }
        if self.path.as_os_str().is_empty() {
            return Ok(());
        }
        let tmp = self.path.with_extension("tmp");
        fs::write(
            &tmp,
            serde_json::to_vec(
                &json!({"version":1,"revision":self.revision,"windows":self.windows}),
            )
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
        fs::rename(tmp, &self.path).map_err(|e| e.to_string())
    }
    pub fn read(&self, id: &str) -> Value {
        let mut record = self
            .windows
            .get(id)
            .cloned()
            .unwrap_or_else(self.profile.default_record);
        record["detached"] = json!(
            self.windows
                .values()
                .any(|v| v["owner"] == id && v["ready"] == true && v["open"] == true)
        );
        json!({"revision":self.revision,"id":id,"layout":record})
    }
    fn patch(&mut self, id: &str, expected: u64, patch: &Value) -> Result<(), String> {
        if let Some(error) = &self.load_error {
            return Err(error.clone());
        }
        if expected != self.revision {
            return Err("WORKSPACE_CONFLICT".into());
        }
        (self.profile.validate)(patch)?;
        let values = patch.as_object().ok_or("Invalid workspace patch")?;
        let record = self
            .windows
            .entry(id.into())
            .or_insert_with(self.profile.default_record);
        for (key, value) in values {
            record[key] = value.clone();
        }
        let group = record["linkGroup"].as_str().unwrap_or("").to_owned();
        let linked = self
            .profile
            .linked_fields
            .iter()
            .map(|key| (*key, record[*key].clone()))
            .collect::<Vec<_>>();
        if !group.is_empty()
            && self
                .profile
                .linked_fields
                .iter()
                .any(|key| values.contains_key(*key))
        {
            for (other, row) in self.windows.iter_mut() {
                if other != id && row["linkGroup"] == group && row["locked"] != true {
                    for (key, value) in &linked {
                        row[*key] = value.clone();
                    }
                }
            }
        }
        self.revision += 1;
        self.save()
    }
    fn close(&mut self, id: &str) {
        if let Some(record) = self.windows.get_mut(id) {
            record["open"] = json!(false);
            record["ready"] = json!(false);
        }
        self.revision += 1;
    }
}
fn decode_store(
    bytes: &[u8],
    profile: &'static WindowProfile,
) -> Result<(u64, BTreeMap<String, Value>), String> {
    let invalid = "工作区格式不受支持或已损坏，原始文件已保留";
    let value: Value = serde_json::from_slice(bytes).map_err(|_| invalid.to_string())?;
    if value["version"] != 1 {
        return Err(invalid.into());
    }
    let revision = value["revision"].as_u64().ok_or(invalid)?;
    let rows = value["windows"].as_object().ok_or(invalid)?;
    if !rows.contains_key("main") {
        return Err(invalid.into());
    }
    let mut windows = BTreeMap::new();
    for (id, row) in rows {
        if !(id == "main"
            || (id.starts_with("workspace-")
                && id.len() <= 80
                && id.chars().all(|c| c.is_ascii_alphanumeric() || c == '-')))
        {
            return Err(invalid.into());
        }
        let fields = row.as_object().ok_or(invalid)?;
        if (profile.default_record)()
            .as_object()
            .unwrap()
            .keys()
            .any(|k| !fields.contains_key(k))
            || !["workspace", profile.detached_kind]
                .iter()
                .any(|kind| row["kind"] == *kind)
            || row["owner"].as_str().is_none()
            || !row["ready"].is_boolean()
            || !row["open"].is_boolean()
        {
            return Err(invalid.into());
        }
        let mut patch = row.clone();
        let fields = patch.as_object_mut().unwrap();
        for key in ["kind", "owner", "ready", "open", "placement"] {
            fields.remove(key);
        }
        (profile.validate)(&patch).map_err(|_| invalid.to_string())?;
        let mut row = row.clone();
        row["ready"] = json!(false);
        windows.insert(id.clone(), row);
    }
    Ok((revision, windows))
}
fn launch(app: &AppHandle, id: &str, record: &Value) -> Result<(), String> {
    let title = {
        let state = app.state::<Workspace>();
        let store = state.0.lock().map_err(|e| e.to_string())?;
        (store.profile.title)(record)
    };
    let win = if let Some(existing) = app.get_webview_window(id) {
        existing
    } else {
        WebviewWindowBuilder::new(app, id, WebviewUrl::App("index.html".into()))
            .decorations(!cfg!(target_os = "linux"))
            .title(title)
            .inner_size(1100.0, 760.0)
            .min_inner_size(800.0, 600.0)
            .center()
            .build()
            .map_err(|e| e.to_string())?
    };
    if let Some(rect) = record["placement"].as_object() {
        let monitors = win.available_monitors().map_err(|e| e.to_string())?;
        let preferred = rect.get("monitor").and_then(Value::as_str);
        let monitor = monitors
            .iter()
            .find(|m| m.name().map(String::as_str) == preferred)
            .or_else(|| monitors.first());
        if let Some(monitor) = monitor {
            let scale = monitor.scale_factor();
            let area = monitor.work_area();
            let number = |key: &str, fallback: f64| {
                rect.get(key)
                    .and_then(Value::as_f64)
                    .filter(|n| n.is_finite())
                    .unwrap_or(fallback)
            };
            let w = (number("width", 1100.0).clamp(800.0, 5000.0) * scale)
                .min(area.size.width as f64) as u32;
            let h = (number("height", 760.0).clamp(600.0, 4000.0) * scale)
                .min(area.size.height as f64) as u32;
            let x = area.position.x
                + (number("x", 0.0) * scale).clamp(0.0, (area.size.width - w) as f64) as i32;
            let y = area.position.y
                + (number("y", 0.0) * scale).clamp(0.0, (area.size.height - h) as f64) as i32;
            win.set_size(tauri::PhysicalSize::new(w, h))
                .map_err(|e| e.to_string())?;
            win.set_position(tauri::PhysicalPosition::new(x, y))
                .map_err(|e| e.to_string())?;
        }
    }
    Ok(())
}
pub fn reopen_main(app: &AppHandle) -> Result<(), String> {
    let row = {
        let state = app.state::<Workspace>();
        let mut store = state.0.lock().map_err(|e| e.to_string())?;
        store.quitting = false;
        store
            .windows
            .get("main")
            .cloned()
            .unwrap_or_else(store.profile.default_record)
    };
    launch(app, "main", &row)?;
    if let Some(window) = app.get_webview_window("main") {
        window.unminimize().map_err(|e| e.to_string())?;
        window.show().map_err(|e| e.to_string())?;
        window.set_focus().map_err(|e| e.to_string())?;
    }
    Ok(())
}
pub fn initialize(app: &AppHandle) -> Result<(), String> {
    let dir = app.path().app_data_dir().map_err(|e| e.to_string())?;
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let state = app.state::<Workspace>();
    let restore;
    {
        let mut s = state.0.lock().map_err(|e| e.to_string())?;
        s.path = dir.join("workspace.json");
        if s.path.exists() {
            let bytes = fs::read(&s.path).map_err(|e| e.to_string())?;
            match decode_store(&bytes, s.profile) {
                Ok((revision, windows)) => {
                    s.revision = revision;
                    s.windows = windows;
                }
                Err(error) => {
                    s.load_error = Some(error);
                }
            }
        }
        let default_record = s.profile.default_record;
        s.windows
            .entry("main".into())
            .or_insert_with(default_record);
        restore = s
            .windows
            .iter()
            .filter(|(id, v)| id.as_str() == "main" || v["open"] == true)
            .map(|(id, v)| (id.clone(), v.clone()))
            .collect::<Vec<_>>();
    }
    for (id, row) in restore {
        if let Err(e) = launch(app, &id, &row) {
            eprintln!("Cannot restore workspace: {e}");
        }
    }
    Ok(())
}
#[tauri::command]
pub fn desktop_workspace_read(
    window: WebviewWindow,
    state: tauri::State<Workspace>,
) -> Result<Value, String> {
    let store = state.0.lock().map_err(|e| e.to_string())?;
    if let Some(error) = &store.load_error {
        return Err(error.clone());
    }
    Ok(store.read(window.label()))
}
#[tauri::command]
pub fn desktop_workspace_patch(
    window: WebviewWindow,
    state: tauri::State<Workspace>,
    expected: u64,
    patch: Value,
) -> Result<Value, String> {
    let mut s = state.0.lock().map_err(|e| e.to_string())?;
    s.patch(window.label(), expected, &patch)?;
    Ok(s.read(window.label()))
}
#[tauri::command]
pub fn desktop_workspace_open(
    app: AppHandle,
    window: WebviewWindow,
    state: tauri::State<Workspace>,
    id: String,
    kind: String,
) -> Result<(), String> {
    if !id.starts_with("workspace-")
        || id.len() > 80
        || !id.chars().all(|c| c.is_ascii_alphanumeric() || c == '-')
    {
        return Err("Invalid window".into());
    }
    let row;
    {
        let mut s = state.0.lock().map_err(|e| e.to_string())?;
        if kind != "workspace" && kind != s.profile.detached_kind {
            return Err("Invalid window kind".into());
        }
        if s.windows.contains_key(&id) {
            return Err("Window already registered".into());
        }
        if kind == s.profile.detached_kind
            && s.windows
                .values()
                .any(|r| r["owner"] == window.label() && r["open"] == true)
        {
            return Err(s.profile.duplicate_detached_error.into());
        }
        let mut r = s
            .windows
            .get(window.label())
            .cloned()
            .unwrap_or_else(s.profile.default_record);
        r["kind"] = json!(kind);
        r["owner"] = json!(if kind == s.profile.detached_kind {
            window.label()
        } else {
            ""
        });
        r["ready"] = json!(false);
        r["open"] = json!(true);
        r.as_object_mut().unwrap().remove("placement");
        if kind == s.profile.detached_kind {
            (s.profile.prepare_child)(&mut r);
        }
        s.windows.insert(id.clone(), r.clone());
        s.revision += 1;
        row = r;
    }
    if let Err(e) = launch(&app, &id, &row) {
        let mut s = state.0.lock().map_err(|e| e.to_string())?;
        s.windows.remove(&id);
        s.revision += 1;
        return Err(e);
    }
    Ok(())
}
#[tauri::command]
pub fn desktop_workspace_ready(
    window: WebviewWindow,
    state: tauri::State<Workspace>,
) -> Result<(), String> {
    let mut s = state.0.lock().map_err(|e| e.to_string())?;
    let default_record = s.profile.default_record;
    let r = s
        .windows
        .entry(window.label().into())
        .or_insert_with(default_record);
    if r["ready"] != true {
        r["ready"] = json!(true);
        r["open"] = json!(true);
        s.revision += 1;
        s.save()?;
    }
    Ok(())
}
#[tauri::command]
pub fn desktop_workspace_merge(
    app: AppHandle,
    window: WebviewWindow,
    state: tauri::State<Workspace>,
) -> Result<(), String> {
    let child;
    {
        let mut s = state.0.lock().map_err(|e| e.to_string())?;
        let own = s
            .windows
            .get(window.label())
            .cloned()
            .ok_or("Window missing")?;
        child = if own["kind"] == s.profile.detached_kind {
            window.label().to_owned()
        } else {
            s.windows
                .iter()
                .find(|(_, r)| r["owner"] == window.label() && r["open"] == true)
                .map(|(id, _)| id.clone())
                .ok_or(s.profile.missing_detached_error)?
        };
        let r = s.windows.get(&child).cloned().ok_or("Panel missing")?;
        let owner = r["owner"].as_str().ok_or("Owner missing")?;
        if app.get_webview_window(owner).is_none() {
            return Err(s.profile.owner_closed_error.into());
        }
        let fields = s.profile.merge_fields;
        if let Some(parent) = s.windows.get_mut(owner) {
            for key in fields {
                parent[*key] = r[*key].clone();
            }
        }
        s.close(&child);
        s.save()?;
    }
    if let Some(win) = app.get_webview_window(&child) {
        win.close().map_err(|e| e.to_string())?;
    }
    Ok(())
}
pub fn window_event(window: &tauri::Window, event: &tauri::WindowEvent) {
    let app = window.app_handle();
    let state = app.state::<Workspace>();
    if let Ok(mut s) = state.0.lock() {
        if !s.windows.contains_key(window.label()) {
            return;
        }
        match event {
            tauri::WindowEvent::Destroyed if !s.quitting => {
                s.close(window.label());
                let _ = s.save();
            }
            tauri::WindowEvent::Moved(_) | tauri::WindowEvent::Resized(_) => {
                if let (Ok(pos), Ok(size)) = (window.outer_position(), window.inner_size())
                    && let (Some(r), Ok(Some(monitor))) =
                        (s.windows.get_mut(window.label()), window.current_monitor())
                {
                    let scale = monitor.scale_factor();
                    let area = monitor.work_area();
                    r["placement"] = json!({"monitor":monitor.name(),"x":(pos.x-area.position.x) as f64/scale,"y":(pos.y-area.position.y) as f64/scale,"width":size.width as f64/scale,"height":size.height as f64/scale});
                    let _ = s.save();
                }
            }
            _ => {}
        }
    };
}
pub fn exit_requested(app: &AppHandle) {
    if let Ok(mut store) = app.state::<Workspace>().0.lock() {
        store.quitting = true;
    }
}
#[cfg(test)]
mod tests {
    use super::*;

    fn default_record() -> Value {
        json!({"selected":"", "source":"", "view":"Overview", "linkGroup":"A",
            "locked":false, "ratio":60, "viewport":null, "dock":"left",
            "kind":"workspace", "owner":"", "ready":false, "open":true})
    }
    fn validate(patch: &Value) -> Result<(), String> {
        for (key, value) in patch.as_object().ok_or("Invalid fixture patch")? {
            let valid = match key.as_str() {
                "selected" | "source" | "view" => value.is_string(),
                "locked" => value.is_boolean(),
                "linkGroup" => value.as_str().is_some_and(|v| ["", "A", "B"].contains(&v)),
                "ratio" => value.as_u64().is_some_and(|v| (30..=75).contains(&v)),
                "dock" => value
                    .as_str()
                    .is_some_and(|v| ["left", "bottom"].contains(&v)),
                "viewport" => {
                    value.is_null()
                        || value["start"]
                            .as_i64()
                            .zip(value["end"].as_i64())
                            .is_some_and(|(a, b)| a < b)
                }
                _ => false,
            };
            if !valid {
                return Err(format!("Invalid fixture field: {key}"));
            }
        }
        Ok(())
    }
    static PROFILE: WindowProfile = WindowProfile {
        default_record,
        validate,
        linked_fields: &["selected", "source"],
        merge_fields: &["selected", "source", "viewport"],
        detached_kind: "detail",
        duplicate_detached_error: "Detail already open",
        missing_detached_error: "Detail missing",
        owner_closed_error: "Owner closed",
        prepare_child: |_| {},
        title: |_| "Fixture",
    };
    fn decode_store(bytes: &[u8]) -> Result<(u64, BTreeMap<String, Value>), String> {
        super::decode_store(bytes, &PROFILE)
    }

    #[test]
    fn generic_linking_accepts_only_fields_from_the_profile() {
        let mut store = Store::new(&PROFILE);
        store.windows.insert("main".into(), default_record());
        store.windows.insert("peer".into(), default_record());
        store
            .patch("main", 0, &json!({"selected":"updated"}))
            .unwrap();
        assert_eq!(store.windows["peer"]["selected"], "updated");
        assert_eq!(store.revision, 1);
        assert!(
            store
                .patch("main", 1, &json!({"unregistered":"value"}))
                .is_err()
        );
        assert_eq!(store.revision, 1);
    }

    #[test]
    fn invalid_current_contract_blocks_persistence() {
        let current = json!({"version":1,"revision":4,"windows":{"main":default_record()}});
        assert_eq!(
            decode_store(&serde_json::to_vec(&current).unwrap())
                .unwrap()
                .0,
            4
        );
        let mut unknown_schema = current.clone();
        unknown_schema["version"] = json!("unknown-schema");
        let mut missing_field = current.clone();
        missing_field["windows"]["main"]
            .as_object_mut()
            .unwrap()
            .remove("selected");
        for data in [unknown_schema, missing_field] {
            assert!(decode_store(&serde_json::to_vec(&data).unwrap()).is_err());
        }
        let mut store = Store {
            load_error: Some("Unsupported workspace".into()),
            ..Store::new(&PROFILE)
        };
        assert!(store.patch("main", 0, &json!({"view":"Data"})).is_err());
        assert!(store.save().is_err());
        assert_eq!(store.revision, 0);
    }

    #[test]
    fn compare_revision_and_respect_link_locks() {
        let mut store = Store::new(&PROFILE);
        store.windows.insert("main".into(), default_record());
        store.windows.insert("peer".into(), default_record());
        store
            .patch("main", 0, &json!({"selected":"item-1","source":"source-1"}))
            .unwrap();
        assert_eq!(store.windows["peer"]["selected"], "item-1");
        assert!(store.patch("peer", 0, &json!({"view":"Data"})).is_err());
        store.patch("peer", 1, &json!({"locked":true})).unwrap();
        store
            .patch("main", 2, &json!({"selected":"item-2"}))
            .unwrap();
        assert_eq!(store.windows["peer"]["selected"], "item-1");
    }

    #[test]
    fn detach_requires_ready_and_close_restores_owner() {
        let mut store = Store::new(&PROFILE);
        store.windows.insert("main".into(), default_record());
        let mut child = default_record();
        child["owner"] = json!("main");
        store.windows.insert("detail".into(), child);
        assert_eq!(store.read("main")["layout"]["detached"], false);
        store.windows.get_mut("detail").unwrap()["ready"] = json!(true);
        assert_eq!(store.read("main")["layout"]["detached"], true);
        store.close("detail");
        assert_eq!(store.read("main")["layout"]["detached"], false);
    }

    #[test]
    fn persisted_layout_and_invalid_patch() {
        let dir = std::env::temp_dir().join(format!("asterion-layout-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let mut store = Store {
            path: dir.join("workspace.json"),
            ..Store::new(&PROFILE)
        };
        assert!(store.patch("main", 0, &json!({"ratio":99})).is_err());
        assert_eq!(store.revision, 0);
        store
            .patch("main", 0, &json!({"ratio":45,"view":"Data"}))
            .unwrap();
        let saved: Value = serde_json::from_slice(&fs::read(&store.path).unwrap()).unwrap();
        assert_eq!(saved["windows"]["main"]["ratio"], 45);
        assert_eq!(saved["windows"]["main"]["view"], "Data");
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn plugin_state_validates_and_moves_with_window_record() {
        let mut store = Store::new(&PROFILE);
        let viewport = json!({"start":-5,"end":80});
        store
            .patch("main", 0, &json!({"viewport":viewport,"dock":"bottom"}))
            .unwrap();
        assert_eq!(store.read("main")["layout"]["viewport"], viewport);
        assert_eq!(store.read("main")["layout"]["dock"], "bottom");
        assert!(
            store
                .patch("main", 1, &json!({"viewport":{"start":10,"end":5}}))
                .is_err()
        );
        assert_eq!(store.revision, 1);
        assert_eq!(store.windows["main"]["viewport"], viewport);
    }
}
