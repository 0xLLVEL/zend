mod core;

use std::sync::Arc;

use serde::Serialize;
use tauri::{Manager, State};

struct AppState {
    core: Arc<core::Runtime>,
}

#[derive(Serialize)]
struct AppInfo {
    version: &'static str,
    arch: &'static str,
    os: &'static str,
    client_build: &'static str,
    client_version: &'static str,
}

#[tauri::command]
fn app_info() -> AppInfo {
    AppInfo {
        version: env!("CARGO_PKG_VERSION"),
        arch: std::env::consts::ARCH,
        os: std::env::consts::OS,
        client_build: env!("ZEND_CLIENT_BUILD"),
        client_version: env!("ZEND_CLIENT_VERSION"),
    }
}

#[tauri::command]
fn core_snapshot(state: State<'_, AppState>) -> core::Snapshot {
    core::snapshot(&state.core)
}

#[tauri::command]
fn core_exec(
    app: tauri::AppHandle,
    state: State<'_, AppState>,
    source: String,
    chunk: String,
) -> Result<u64, String> {
    core::exec_script(&app, &state.core, source, chunk)
}

#[tauri::command]
fn core_connect(app: tauri::AppHandle, state: State<'_, AppState>) -> Result<(), String> {
    core::connect_now(&app, &state.core)
}

#[tauri::command]
fn core_detach(app: tauri::AppHandle, state: State<'_, AppState>) -> Result<(), String> {
    core::detach(&app, &state.core)
}

#[tauri::command]
fn core_set_pipe(state: State<'_, AppState>, name: String) {
    core::set_pipe(&state.core, &name);
}

#[cfg(windows)]
mod hotkey {
    use std::ffi::c_void;

    use tauri::{AppHandle, Manager};

    const MOD_CONTROL: u32 = 0x0002;
    const MOD_SHIFT: u32 = 0x0004;
    const MOD_NOREPEAT: u32 = 0x4000;
    const VK_Z: u32 = 0x5A;
    const WM_HOTKEY: u32 = 0x0312;

    #[repr(C)]
    struct Point {
        x: i32,
        y: i32,
    }

    #[repr(C)]
    struct Msg {
        hwnd: *mut c_void,
        message: u32,
        w_param: usize,
        l_param: isize,
        time: u32,
        point: Point,
    }

    unsafe extern "system" {
        fn RegisterHotKey(hwnd: *mut c_void, id: i32, modifiers: u32, vk: u32) -> i32;
        fn UnregisterHotKey(hwnd: *mut c_void, id: i32) -> i32;
        fn GetMessageW(msg: *mut Msg, hwnd: *mut c_void, min: u32, max: u32) -> i32;
    }

    pub fn install(app: AppHandle) {
        std::thread::Builder::new()
            .name("zend-hotkey".into())
            .spawn(move || unsafe {
                if RegisterHotKey(
                    std::ptr::null_mut(),
                    1,
                    MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
                    VK_Z,
                ) == 0
                {
                    return;
                }

                let mut msg: Msg = std::mem::zeroed();
                while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
                    if msg.message != WM_HOTKEY {
                        continue;
                    }
                    let Some(window) = app.get_webview_window("main") else {
                        continue;
                    };
                    let visible = window.is_visible().unwrap_or(true);
                    let minimized = window.is_minimized().unwrap_or(false);
                    if minimized {
                        let _ = window.unminimize();
                        let _ = window.show();
                        let _ = window.set_focus();
                    } else if visible {
                        let _ = window.hide();
                    } else {
                        let _ = window.show();
                        let _ = window.set_focus();
                    }
                }

                UnregisterHotKey(std::ptr::null_mut(), 1);
            })
            .ok();
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .setup(|app| {
            let runtime = Arc::new(core::Runtime::default());
            core::start(app.handle().clone(), runtime.clone());
            app.manage(AppState { core: runtime });
            #[cfg(windows)]
            hotkey::install(app.handle().clone());
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            app_info,
            core_snapshot,
            core_exec,
            core_connect,
            core_detach,
            core_set_pipe
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
