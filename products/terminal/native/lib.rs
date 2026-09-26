//! Approved native contributions for the default product.
use asterion_desktop_bridge::{
    window::{NativeWindows, SettingsWindow, WindowSize},
    workspace::WindowProfile,
};

pub fn workspace_profile() -> &'static WindowProfile {
    &asterion_ui_terminal_workspace::PROFILE
}

pub fn native_windows() -> NativeWindows {
    NativeWindows {
        settings: SettingsWindow {
            label: "settings",
            route: "index.html?screen=settings",
            title: "设置 · Asterion Terminal",
            size: WindowSize {
                width: 800.0,
                height: 580.0,
            },
            minimum: WindowSize {
                width: 680.0,
                height: 460.0,
            },
        },
        setup: WindowSize {
            width: 800.0,
            height: 640.0,
        },
        workspace: WindowSize {
            width: 1440.0,
            height: 940.0,
        },
        resize_minimum: WindowSize {
            width: 640.0,
            height: 580.0,
        },
    }
}
