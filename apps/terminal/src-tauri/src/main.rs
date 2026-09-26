#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
use asterion_kernel::{communication, transport};
use asterion_foundation::wire_generated;
mod runtime_setup;
mod network_stats;
use std::sync::Mutex;
use runtime_setup::{Progress, Setup};
use asterion_desktop_bridge::{workspace, session, window};
use asterion_product_terminal as distribution;
use asterion_desktop_bridge::workspace::Workspace;
use asterion_desktop_bridge::session::AccountSession;
use asterion_desktop_bridge::window::show_settings;
use std::process::Command;
use tauri::{AppHandle, Manager};
#[cfg(target_os = "macos")]
use tauri::menu::{Menu, MenuItem, MenuItemKind};

fn backend_call(app: &AppHandle, role: &str, context: &wire_generated::Context) -> Result<serde_json::Value, String> {
    backend_request(app, role, &[], context)
}

fn backend_request(app: &AppHandle, role: &str, extra: &[String], context: &wire_generated::Context) -> Result<serde_json::Value, String> {
    let resources = app.path().resource_dir().map_err(|e| e.to_string())?;
    let state = app.path().app_data_dir().map_err(|e| e.to_string())?;
    let setup = Setup::load(&resources.join("setup"), &state)?;
    let lease = setup.runtime_lease()?;
    let mut command = Command::new(setup.python());
    command.args(["-I", "-m", "asterion.runtime.cli"])
        .arg(role).args(extra)
        .arg("--communication-context").arg(serde_json::to_string(&context).map_err(|e| e.to_string())?)
        .arg("--state").arg(state)
        .arg("--pg-root").arg(setup.postgres_root())
        .env("PYTHONDONTWRITEBYTECODE", "1")
        .stdin(lease.try_clone().map_err(|e| e.to_string())?);
    let output = transport::capture(command, context).map_err(|e| e.to_string())?;
    if !output.status.success() {
        return communication::decode_reply(context, &output.stdout).map_err(|e| e.to_string()).and_then(|_| Err("本机操作异常终止".into()));
    }
    communication::decode_reply(context, &output.stdout).map_err(|e| e.to_string())
}

#[tauri::command]
async fn desktop_session(app: AppHandle, communication: wire_generated::Context) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || backend_call(&app, "desktop-bootstrap", &communication))
        .await.map_err(|e| e.to_string())?
}

#[derive(Default)]
struct SetupState(Mutex<Progress>);
fn runtime_setup(app: &AppHandle) -> Result<Setup, String> {
    Setup::load(&app.path().resource_dir().map_err(|e| e.to_string())?.join("setup"), &app.path().app_data_dir().map_err(|e| e.to_string())?)
}
#[tauri::command]
fn desktop_setup_status(app: AppHandle) -> Result<Progress, String> {
    let setup = runtime_setup(&app)?;
    let mut progress = app.state::<SetupState>().0.lock().map_err(|e| e.to_string())?.clone();
    progress.network = if progress.running { network_stats::snapshot() } else { None };
    progress.ready = setup.ready();
    progress.directory = setup.root.display().to_string();
    Ok(progress)
}
#[tauri::command]
async fn desktop_setup_install(app: AppHandle) -> Result<Progress, String> {
    let setup = runtime_setup(&app)?;
    {
        let state = app.state::<SetupState>();
        let mut progress = state.0.lock().map_err(|e| e.to_string())?;
        if progress.running { return Err("正在安装，请等待完成".into()); }
        *progress = Progress { running: true, directory: setup.root.display().to_string(), ..Default::default() };
    }
    let handle = app.clone();
    let result = tauri::async_runtime::spawn_blocking(move || setup.install(|p| {
        if let Ok(mut value) = handle.state::<SetupState>().0.lock() { *value = p; }
    })).await.map_err(|e| e.to_string()).and_then(|r| r);
    let state = app.state::<SetupState>();
    let mut progress = state.0.lock().map_err(|e| e.to_string())?;
    progress.running = false;
    match result {
        Ok(()) => { progress.ready = true; progress.step = 5; progress.error.clear(); }
        Err(error) => { progress.error = error; }
    }
    Ok(progress.clone())
}
#[tauri::command]
async fn desktop_stop(app: AppHandle, communication: wire_generated::Context) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || backend_call(&app, "desktop-stop", &communication))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn desktop_backup(app: AppHandle, communication: wire_generated::Context) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || backend_call(&app, "desktop-backup", &communication))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn desktop_restore(app: AppHandle, archive: String, target: String, communication: wire_generated::Context) -> Result<serde_json::Value, String> {
    if !std::path::Path::new(&archive).is_absolute() || !std::path::Path::new(&target).is_absolute() {
        return Err("请输入备份和新恢复目录的完整路径".into());
    }
    tauri::async_runtime::spawn_blocking(move || backend_request(&app, "desktop-restore", &["--archive".into(), archive, "--target".into(), target], &communication))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn desktop_info(app: AppHandle, communication: wire_generated::Context) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || backend_call(&app, "desktop-info", &communication))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn desktop_environment(app: AppHandle, communication: wire_generated::Context) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || backend_call(&app, "desktop-environment", &communication))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn desktop_activate(app: AppHandle, target: Option<String>, communication: wire_generated::Context) -> Result<serde_json::Value, String> {
    if target.as_ref().is_some_and(|path| !std::path::Path::new(path).is_absolute()) {
        return Err("请输入恢复目录的完整路径".into());
    }
    let handle = app.clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        if let Some(path) = target {
            backend_request(&handle, "desktop-activate", &["--target".into(), path], &communication)
        } else {
            backend_call(&handle, "desktop-rollback", &communication)
        }
    }).await.map_err(|e| e.to_string())?;
    if result.is_err() { return result; }
    session::invalidate_windows(&app);
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
        .manage(SetupState::default())
        .manage(AccountSession::default())
        .manage(distribution::native_windows())
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
        .invoke_handler({
            let dispatch: Box<dyn Fn(tauri::ipc::Invoke<tauri::Wry>) -> bool + Send + Sync> = Box::new(tauri::generate_handler![desktop_setup_status, desktop_setup_install, window::desktop_setup_window, desktop_session, desktop_stop, desktop_backup, desktop_restore, desktop_info, desktop_environment, desktop_activate, window::open_settings, session::desktop_account_read, session::desktop_account_write, window::desktop_window_id, workspace::desktop_workspace_read, workspace::desktop_workspace_patch, workspace::desktop_workspace_open, workspace::desktop_workspace_ready, workspace::desktop_workspace_merge]);
            move |invoke: tauri::ipc::Invoke<tauri::Wry>| {
                if let Err(error) = asterion_desktop_bridge::communication::native_context(&invoke) {
                    invoke.resolver.reject(error);
                    return true;
                }
                dispatch(invoke)
            }
        })
        .build(tauri::generate_context!())
        .expect("failed to build Asterion desktop host")
         .run(|app, event| {
            if matches!(event, tauri::RunEvent::ExitRequested { .. }) {
                workspace::exit_requested(app);
            }
            #[cfg(target_os = "macos")]
            if matches!(event, tauri::RunEvent::Reopen { has_visible_windows: false, .. }) {
                if let Err(error)=workspace::reopen_main(app) { eprintln!("Cannot reopen workspace: {error}"); }
            }
        });
}
