//! Tauri value extraction; runtime admission is owned by the fixed L1 kernel.
use asterion_kernel::communication::{ensure_active, parse_context};

pub fn native_context<R: tauri::Runtime>(invoke: &tauri::ipc::Invoke<R>) -> Result<(), String> {
    let tauri::ipc::InvokeBody::Json(payload) = invoke.message.payload() else {
        return Err("原生通信须使用 JSON".into());
    };
    let trace = payload
        .get("communication")
        .ok_or("原生调用缺少通信上下文")?;
    let trace = parse_context(trace.clone()).map_err(|e| e.to_string())?;
    ensure_active(&trace).map_err(|e| e.to_string())?;
    Ok(())
}
