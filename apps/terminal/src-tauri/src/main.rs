#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
mod session;
mod workspace;
mod plugins;
mod distribution;
use workspace::{Workspace, desktop_workspace_read, desktop_workspace_patch, desktop_workspace_open, desktop_workspace_ready, desktop_workspace_merge};
use session::{AccountSession, desktop_account_read, desktop_account_write, desktop_window_id};
use std::process::Command;
use tauri::{AppHandle, Manager, WebviewUrl, WebviewWindowBuilder};
#[cfg(target_os = "macos")]
use tauri::menu::{Menu, MenuItem, MenuItemKind};

fn backend_call(app: &AppHandle, role: &str) -> Result<serde_json::Value, String> {
    backend_request(app, role, &[])
}

fn backend_request(app: &AppHandle, role: &str, extra: &[String]) -> Result<serde_json::Value, String> {
    let resources = app.path().resource_dir().map_err(|e| e.to_string())?;
    let state = app.path().app_data_dir().map_err(|e| e.to_string())?;
    let runtime = resources.join("runtime");
    let output = Command::new(runtime.join("asterion-backend/asterion-backend"))
        .arg(role).args(extra)
        .arg("--state").arg(state)
        .arg("--pg-root").arg(runtime.join("postgres"))
        .env("PYINSTALLER_RESET_ENVIRONMENT", "1")
        .output().map_err(|e| format!("无法启动本机运行环境：{e}"))?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr).chars().take(3000).collect());
    }
    if role == "desktop-stop" { return Ok(serde_json::json!({"status": "stopped"})); }
    serde_json::from_slice(&output.stdout).map_err(|_| "本机服务没有返回有效会话，请重试".into())
}

#[tauri::command]
async fn desktop_session(app: AppHandle) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || backend_call(&app, "desktop-bootstrap"))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn desktop_stop(app: AppHandle) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || backend_call(&app, "desktop-stop"))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn desktop_backup(app: AppHandle) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || backend_call(&app, "desktop-backup"))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn desktop_restore(app: AppHandle, archive: String, target: String) -> Result<serde_json::Value, String> {
    if !std::path::Path::new(&archive).is_absolute() || !std::path::Path::new(&target).is_absolute() {
        return Err("请输入备份和新恢复目录的完整路径".into());
    }
    tauri::async_runtime::spawn_blocking(move || backend_request(&app, "desktop-restore", &["--archive".into(), archive, "--target".into(), target]))
        .await.map_err(|e| e.to_string())?
}

fn show_settings(app: &AppHandle) -> Result<(), String> {
    if let Some(window) = app.get_webview_window("settings") {
        window.unminimize().map_err(|e| e.to_string())?;
        return window.set_focus().map_err(|e| e.to_string());
    }
    WebviewWindowBuilder::new(app, "settings", WebviewUrl::App("index.html?screen=settings".into()))
        .disable_drag_drop_handler()
        .decorations(!cfg!(target_os = "linux"))
        .title("设置 · Asterion Terminal").inner_size(800.0, 580.0)
        .min_inner_size(680.0, 460.0).center().build().map_err(|e| e.to_string())?;
    Ok(())
}

#[tauri::command]
fn open_settings(app: AppHandle) -> Result<(), String> { show_settings(&app) }

#[tauri::command]
async fn desktop_info(app: AppHandle) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || backend_call(&app, "desktop-info"))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn desktop_environment(app: AppHandle) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || backend_call(&app, "desktop-environment"))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn desktop_activate(app: AppHandle, target: Option<String>) -> Result<serde_json::Value, String> {
    if target.as_ref().is_some_and(|path| !std::path::Path::new(path).is_absolute()) {
        return Err("请输入恢复目录的完整路径".into());
    }
    let handle = app.clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        if let Some(path) = target {
            backend_request(&handle, "desktop-activate", &["--target".into(), path])
        } else {
            backend_call(&handle, "desktop-rollback")
        }
    }).await.map_err(|e| e.to_string())?;
    if result.is_err() { return result; }
    // Revoke shared native authentication and discard every window's stale endpoint and drafts.
    if let Ok(mut session) = app.state::<AccountSession>().0.lock() {
        session.token = None;
        session.revision += 1;
    }
    for window in app.webview_windows().values() {
        if let Err(error) = window.eval("window.location.reload()") {
            eprintln!("Cannot refresh environment window: {error}");
        }
    }
    result
}

fn main() {
    #[cfg(target_os = "linux")]
    if std::env::var_os("WEBKIT_DISABLE_DMABUF_RENDERER").is_none()
        && std::path::Path::new("/sys/module/nvidia").exists()
    {
        // WebKitGTK's DMABUF path can fail to allocate GBM buffers on NVIDIA.
        // Set this before GTK starts threads, and preserve explicit user overrides.
        std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
    }
    tauri::Builder::default()
        .manage(AccountSession::default())
        .manage(Workspace::new(distribution::workspace_profile()))
        .on_window_event(workspace::window_event)
        .setup(|app| {
            #[cfg(target_os = "macos")]
            {
                let menu = Menu::default(app.handle())?;
                let settings = MenuItem::with_id(app, "settings", "设置…", true, Some("CmdOrCtrl+,"))?;
                if let Some(MenuItemKind::Submenu(submenu)) = menu.items()?.first() {
                    submenu.insert(&settings, 2)?;
                }
                app.set_menu(menu)?;
            }
            #[cfg(target_os = "linux")]
            if let Some(window) = app.get_webview_window("main") {
                window.set_decorations(false)?;
            }
            if let Err(e) = workspace::initialize(app.handle()) { eprintln!("Cannot restore workspace: {e}"); }
            Ok(())
        })
        .on_menu_event(|app, event| {
            if event.id().as_ref() == "settings" {
                if let Err(error) = show_settings(app) { eprintln!("Cannot open settings: {error}"); }
            }
        })
        .invoke_handler(tauri::generate_handler![desktop_session, desktop_stop, desktop_backup, desktop_restore, desktop_info, desktop_environment, desktop_activate, open_settings, desktop_account_read, desktop_account_write, desktop_window_id, desktop_workspace_read, desktop_workspace_patch, desktop_workspace_open, desktop_workspace_ready, desktop_workspace_merge])
        .build(tauri::generate_context!())
        .expect("failed to build Asterion desktop host")
         .run(|app, event| {
            if matches!(event, tauri::RunEvent::ExitRequested { .. }) {
                if let Ok(mut s) = app.state::<Workspace>().0.lock() { s.quitting = true; }
            }
            #[cfg(target_os = "macos")]
            if matches!(event, tauri::RunEvent::Reopen { has_visible_windows: false, .. }) {
                if let Err(error)=workspace::reopen_main(app) { eprintln!("Cannot reopen workspace: {error}"); }
            }
        });
}
