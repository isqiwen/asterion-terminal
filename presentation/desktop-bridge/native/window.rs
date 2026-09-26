//! Native window operations; dimensions, routes and titles come from L5 assembly.
use tauri::{AppHandle, Manager, WebviewUrl, WebviewWindow, WebviewWindowBuilder};

pub struct WindowSize {
    pub width: f64,
    pub height: f64,
}

pub struct SettingsWindow {
    pub label: &'static str,
    pub route: &'static str,
    pub title: &'static str,
    pub size: WindowSize,
    pub minimum: WindowSize,
}

pub struct NativeWindows {
    pub settings: SettingsWindow,
    pub setup: WindowSize,
    pub workspace: WindowSize,
    pub resize_minimum: WindowSize,
}

#[tauri::command]
pub fn desktop_window_id(window: WebviewWindow) -> String {
    window.label().to_owned()
}

#[tauri::command]
pub fn desktop_setup_window(
    window: WebviewWindow,
    state: tauri::State<NativeWindows>,
    setup: bool,
) -> Result<(), String> {
    let minimum = &state.resize_minimum;
    let size = if setup {
        &state.setup
    } else {
        &state.workspace
    };
    window
        .set_min_size(Some(tauri::LogicalSize::new(minimum.width, minimum.height)))
        .map_err(|e| e.to_string())?;
    window
        .set_size(tauri::LogicalSize::new(size.width, size.height))
        .map_err(|e| e.to_string())?;
    window.center().map_err(|e| e.to_string())
}

pub fn show_settings(app: &AppHandle) -> Result<(), String> {
    let state = app.state::<NativeWindows>();
    let settings = &state.settings;
    if let Some(window) = app.get_webview_window(settings.label) {
        window.unminimize().map_err(|e| e.to_string())?;
        return window.set_focus().map_err(|e| e.to_string());
    }
    WebviewWindowBuilder::new(app, settings.label, WebviewUrl::App(settings.route.into()))
        .disable_drag_drop_handler()
        .decorations(!cfg!(target_os = "linux"))
        .title(settings.title)
        .inner_size(settings.size.width, settings.size.height)
        .min_inner_size(settings.minimum.width, settings.minimum.height)
        .center()
        .build()
        .map_err(|e| e.to_string())?;
    Ok(())
}

#[tauri::command]
pub fn open_settings(app: AppHandle) -> Result<(), String> {
    show_settings(&app)
}
