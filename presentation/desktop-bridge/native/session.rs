use std::sync::Mutex;
use tauri::Manager;

#[derive(Default)]
pub struct AccountSession(Mutex<Session>);
#[derive(Default)]
struct Session {
    revision: u64,
    token: Option<String>,
}
impl Session {
    pub fn replace(&mut self, expected: u64, token: Option<String>) -> Result<(), String> {
        if self.revision != expected {
            return Err("账号状态已在其他窗口改变，请重试".into());
        }
        self.token = token;
        self.revision += 1;
        Ok(())
    }
}
#[tauri::command]
pub fn desktop_account_read(
    state: tauri::State<AccountSession>,
) -> Result<serde_json::Value, String> {
    let session = state.0.lock().map_err(|e| e.to_string())?;
    Ok(serde_json::json!({"revision": session.revision, "token": session.token}))
}
#[tauri::command]
pub fn desktop_account_write(
    state: tauri::State<AccountSession>,
    expected: u64,
    token: Option<String>,
) -> Result<(), String> {
    state
        .0
        .lock()
        .map_err(|e| e.to_string())?
        .replace(expected, token)
}
/// Discard cached UI authentication and drafts after the host switches environments.
pub fn invalidate_windows(app: &tauri::AppHandle) {
    if let Ok(mut session) = app.state::<AccountSession>().0.lock() {
        session.token = None;
        session.revision += 1;
    }
    for window in app.webview_windows().values() {
        if let Err(error) = window.eval("window.location.reload()") {
            eprintln!("Cannot refresh environment window: {error}");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn stale_login_cannot_undo_logout() {
        let mut s = Session::default();
        s.replace(0, Some("first".into())).unwrap();
        let before_logout = s.revision;
        s.replace(before_logout, None).unwrap();
        assert!(s.replace(before_logout, Some("late-login".into())).is_err());
        assert!(s.token.is_none());
    }
    #[test]
    fn stale_logout_cannot_clear_new_login() {
        let mut s = Session::default();
        s.replace(0, Some("first".into())).unwrap();
        s.replace(1, Some("second".into())).unwrap();
        assert!(s.replace(1, None).is_err());
        assert_eq!(s.token.as_deref(), Some("second"));
    }
}
