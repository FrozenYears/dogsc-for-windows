#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

#[tauri::command]
fn app_version() -> &'static str {
    "0.1.0"
}

#[tauri::command]
fn preview_host_hwnd(window: tauri::WebviewWindow) -> Result<u64, String> {
    window.hwnd().map(|hwnd| hwnd.0 as usize as u64).map_err(|error| error.to_string())
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .invoke_handler(tauri::generate_handler![app_version, preview_host_hwnd])
        .run(tauri::generate_context!())
        .expect("DogSC Tauri 应用启动失败");
}
