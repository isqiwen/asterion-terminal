#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
use tauri::Manager;
use std::{
    ffi::{c_char, c_void, CStr, CString},
    sync::Arc,
};

extern "C" {
    fn asterion_terminal_create() -> *mut c_void;
    fn asterion_terminal_call(runtime: *mut c_void, request: *const c_char) -> *mut c_char;
    fn asterion_terminal_free(response: *mut c_char);
    fn asterion_terminal_destroy(runtime: *mut c_void);
}
struct Runtime(*mut c_void);
// The C ABI is thread-safe for concurrent calls: the C++ runtime serializes
// operations itself and answers status reads from its last snapshot while a
// long operation runs, so one slow service cannot freeze every window. The
// pointer is only destroyed by Drop, after all Arc clones are gone.
unsafe impl Send for Runtime {}
unsafe impl Sync for Runtime {}
impl Runtime {
    fn new() -> Result<Self, String> {
        let pointer = unsafe { asterion_terminal_create() };
        if pointer.is_null() {
            Err("无法启动 C++ 核心".into())
        } else {
            Ok(Self(pointer))
        }
    }
    fn request(&self, request: &str) -> Result<String, String> {
        if request.len() > 65536 {
            return Err("请求过大".into());
        }
        let request = CString::new(request).map_err(|_| "请求含有非法字符")?;
        let response = unsafe { asterion_terminal_call(self.0, request.as_ptr()) };
        if response.is_null() {
            return Err("C++ 核心无法分配响应".into());
        }
        let result = unsafe { CStr::from_ptr(response) }
            .to_str()
            .map(str::to_owned)
            .map_err(|_| "核心返回非法 UTF-8".into());
        unsafe { asterion_terminal_free(response) };
        result
    }
}
impl Drop for Runtime {
    fn drop(&mut self) {
        unsafe { asterion_terminal_destroy(self.0) }
    }
}
type SharedRuntime = Arc<Runtime>;

#[tauri::command]
async fn terminal_request(
    state: tauri::State<'_, SharedRuntime>,
    request: String,
) -> Result<String, String> {
    let runtime = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || runtime.request(&request))
    .await
    .map_err(|_| "本机请求任务失败".to_string())?
}

#[tauri::command]
async fn open_settings(app: tauri::AppHandle, category: String) -> Result<(), String> {
    if !["general", "appearance", "connections", "plugins", "about"].contains(&category.as_str()) {
        return Err("invalid settings category".into());
    }
    if let Some(window) = app.get_webview_window("settings") {
        window.unminimize().map_err(|e| e.to_string())?;
        return window.set_focus().map_err(|e| e.to_string());
    }
    tauri::WebviewWindowBuilder::new(&app, "settings", tauri::WebviewUrl::App(format!("index.html?screen=settings&category={category}").into()))
        .title("Asterion Terminal — Settings")
        .inner_size(820.0, 620.0)
        .min_inner_size(700.0, 500.0)
        .disable_drag_drop_handler()
        .decorations(!cfg!(target_os = "linux"))
        .center()
        .build().map_err(|e| e.to_string())?;
    Ok(())
}

fn main() {
    tauri::Builder::default()
        .setup(|app| {
            if !cfg!(debug_assertions) || std::env::var_os("ASTERION_CTP_LIBRARY").is_none() {
                let name = if cfg!(target_os="macos") { "ctp-md.dylib" } else if cfg!(target_os="windows") { "ctp-md.dll" } else { "ctp-md.so" };
                std::env::set_var("ASTERION_CTP_LIBRARY", app.path().resource_dir()?.join("native").join(name));
            }
            if !cfg!(debug_assertions) || std::env::var_os("ASTERION_REMOTE_RESOURCES").is_none() {
                std::env::set_var("ASTERION_REMOTE_RESOURCES", app.path().resource_dir()?.join("remote-linux"));
            }
            app.manage(Arc::new(Runtime::new().map_err(std::io::Error::other)?));
            Ok(())
        })
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![terminal_request, open_settings])
        .run(tauri::generate_context!())
        .expect("Asterion Terminal failed");
}
