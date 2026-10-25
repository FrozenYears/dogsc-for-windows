#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use tauri::menu::{MenuBuilder, MenuItem};
use tauri::tray::{TrayIconBuilder, TrayIconEvent};
use tauri::{Manager, WebviewWindow};
use std::process::Command;

#[tauri::command]
fn app_version() -> &'static str {
    "0.1.0"
}

#[tauri::command]
fn open_windows_settings(page: String) -> Result<(), String> {
    let allowed = [
        "ms-settings:privacy-webcam",
        "ms-settings:privacy-microphone",
        "ms-settings:privacy",
    ];
    if !allowed.contains(&page.as_str()) {
        return Err("不支持的 Windows 设置页面".to_string());
    }
    Command::new("cmd")
        .args(["/C", "start", "", &page])
        .spawn()
        .map(|_| ())
        .map_err(|error| format!("无法打开 Windows 设置：{error}"))
}

#[tauri::command]
fn preview_host_hwnd(window: WebviewWindow) -> Result<u64, String> {
    window.hwnd().map(|hwnd| hwnd.0 as usize as u64).map_err(|error| error.to_string())
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .invoke_handler(tauri::generate_handler![app_version, preview_host_hwnd, open_windows_settings])
        .setup(|app| {
            let show = MenuItem::with_id(app, "show", "打开 DogSC Studio", true, None::<&str>)?;
            let quit = MenuItem::with_id(app, "quit", "退出", true, None::<&str>)?;
            let menu = MenuBuilder::new(app).items(&[&show, &quit]).build()?;
            let mut tray = TrayIconBuilder::new()
                .menu(&menu)
                .tooltip("DogSC Studio")
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "show" => {
                        if let Some(window) = app.get_webview_window("main") {
                            let _ = window.show();
                            let _ = window.set_focus();
                        }
                    }
                    "quit" => app.exit(0),
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click { button, button_state, .. } = event {
                        if button == tauri::tray::MouseButton::Left
                            && button_state == tauri::tray::MouseButtonState::Up
                        {
                            if let Some(window) = tray.app_handle().get_webview_window("main") {
                                let _ = window.show();
                                let _ = window.set_focus();
                            }
                        }
                    }
                });
            if let Some(icon) = app.default_window_icon() {
                tray = tray.icon(icon.clone());
            }
            tray.build(app)?;
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("DogSC Tauri 应用启动失败");
}
