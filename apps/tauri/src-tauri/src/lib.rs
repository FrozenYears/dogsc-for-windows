#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

#[tauri::command]
fn app_version() -> &'static str {
    "0.1.0"
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .invoke_handler(tauri::generate_handler![app_version])
        .run(tauri::generate_context!())
        .expect("DogSC Tauri 应用启动失败");
}
