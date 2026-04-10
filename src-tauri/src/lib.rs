use serde::Serialize;
use std::fs;
use std::path::PathBuf;
use tauri::Manager;

#[derive(Serialize)]
struct ModelInfo {
    name: String,
    path: String,
    model_json: String,
}

#[tauri::command]
fn list_models(app_handle: tauri::AppHandle) -> Vec<ModelInfo> {
    let models_dir = get_models_dir(&app_handle);
    let mut models = Vec::new();

    if let Ok(entries) = fs::read_dir(&models_dir) {
        for entry in entries.flatten() {
            if entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
                let name = entry.file_name().to_string_lossy().to_string();
                let dir_path = entry.path();

                if let Ok(files) = fs::read_dir(&dir_path) {
                    for file in files.flatten() {
                        let fname = file.file_name().to_string_lossy().to_string();
                        if fname.ends_with(".model3.json") {
                            models.push(ModelInfo {
                                name: name.clone(),
                                path: dir_path.to_string_lossy().to_string(),
                                model_json: fname,
                            });
                            break;
                        }
                    }
                }
            }
        }
    }

    models
}

fn get_models_dir(app_handle: &tauri::AppHandle) -> PathBuf {
    let exe_dir = app_handle
        .path()
        .resource_dir()
        .unwrap_or_else(|_| PathBuf::from("."));

    for candidate in [
        exe_dir.join("models"),
        PathBuf::from("models"),
        PathBuf::from("../models"),
    ] {
        if candidate.exists() {
            return candidate;
        }
    }

    exe_dir.join("models")
}

fn get_models_dir_static() -> PathBuf {
    for candidate in [
        PathBuf::from("models"),
        PathBuf::from("../models"),
    ] {
        if candidate.exists() {
            return candidate;
        }
    }
    PathBuf::from("models")
}

fn mime_for_path(path: &str) -> &'static str {
    if path.ends_with(".png") {
        "image/png"
    } else if path.ends_with(".jpg") || path.ends_with(".jpeg") {
        "image/jpeg"
    } else if path.ends_with(".json") {
        "application/json"
    } else if path.ends_with(".moc3") {
        "application/octet-stream"
    } else if path.ends_with(".wav") {
        "audio/wav"
    } else if path.ends_with(".mp3") {
        "audio/mpeg"
    } else {
        "application/octet-stream"
    }
}

fn error_response(status: u16, body: &str) -> tauri::http::Response<Vec<u8>> {
    tauri::http::Response::builder()
        .status(status)
        .body(body.as_bytes().to_vec())
        .unwrap()
}

pub fn run() {
    std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");

    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_process::init())
        .setup(|app| {
            let window = app.get_webview_window("main").unwrap();

            // On Linux/WebKitGTK, hardware-accelerated compositing causes
            // transparent WebGL frames to stack on top of each other instead
            // of replacing the previous frame. Switching to software
            // compositing fixes this while still allowing WebGL rendering.
            #[cfg(target_os = "linux")]
            {
                window.with_webview(|webview| {
                    use webkit2gtk::{SettingsExt, WebViewExt};
                    if let Some(settings) = webview.inner().settings() {
                        settings.set_hardware_acceleration_policy(
                            webkit2gtk::HardwareAccelerationPolicy::Never,
                        );
                    }
                }).ok();
            }

            Ok(())
        })
        .register_uri_scheme_protocol("model", |_ctx, request| {
            let uri = request.uri().to_string();

            let file_path = uri
                .strip_prefix("model://localhost/")
                .or_else(|| uri.strip_prefix("model://localhost"))
                .unwrap_or("");
            let file_path = percent_decode(file_path);

            let models_dir = get_models_dir_static();
            let full_path = models_dir.join(&file_path);

            let canonical = match full_path.canonicalize() {
                Ok(p) => p,
                Err(e) => {
                    eprintln!("[model://] Not found: {full_path:?} — {e}");
                    return error_response(404, &format!("Not found: {file_path}"));
                }
            };
            let canonical_models = match models_dir.canonicalize() {
                Ok(p) => p,
                Err(_) => return error_response(500, "Models dir error"),
            };
            if !canonical.starts_with(&canonical_models) {
                return error_response(403, "Access denied");
            }

            match fs::read(&canonical) {
                Ok(data) => {
                    let mime = mime_for_path(&file_path);
                    tauri::http::Response::builder()
                        .status(200)
                        .header("Content-Type", mime)
                        .header("Access-Control-Allow-Origin", "*")
                        .body(data)
                        .unwrap()
                }
                Err(e) => {
                    eprintln!("[model://] Read error: {canonical:?} — {e}");
                    error_response(500, &format!("Read error: {e}"))
                }
            }
        })
        .invoke_handler(tauri::generate_handler![list_models])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

fn percent_decode(s: &str) -> String {
    let mut result = Vec::new();
    let bytes = s.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let Ok(val) = u8::from_str_radix(
                std::str::from_utf8(&bytes[i + 1..i + 3]).unwrap_or(""),
                16,
            ) {
                result.push(val);
                i += 3;
                continue;
            }
        }
        result.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&result).to_string()
}
