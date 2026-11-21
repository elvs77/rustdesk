//! elvs (16.2): Techniker-Modus.
//!
//! Without an unlock the main window shows the customer view (the "incoming only" layout:
//! logo, ID, password). The technician unlocks the full client with the 6-digit code of his
//! authenticator app. The code is checked by the own server (https://<rendezvous host>/elvs/techniker/),
//! which keeps the authenticator secret; the client only stores the random token it returns.
//!
//! - "dauerhaft" (own computers): the token stays until it is removed; the server drops it after
//!   30 days without use.
//! - otherwise (customer PCs): the unlock ends 10 minutes after the last activity in the client
//!   window and the customer view is back.

use hbb_common::{config::LocalConfig, log};
use std::{
    sync::{
        atomic::{AtomicBool, Ordering},
        Mutex,
    },
    time::{Instant, SystemTime, UNIX_EPOCH},
};

const OPT_TOKEN: &str = "elvs-tech-token";
const OPT_PERMANENT: &str = "elvs-tech-dauerhaft";
const OPT_LAST: &str = "elvs-tech-zuletzt";
const OPT_TECH: &str = "elvs-tech-techniker";
const OPT_NAME: &str = "elvs-tech-name";
const IDLE_SECS: u64 = 10 * 60;
const TOUCH_EVERY_SECS: u64 = 30;
const CHECK_EVERY_SECS: u64 = 5 * 60;

#[derive(Default)]
struct Status {
    busy: bool,
    error: String,
}

lazy_static::lazy_static! {
    static ref STATUS: Mutex<Status> = Default::default();
    static ref LAST_CHECK: Mutex<Option<Instant>> = Default::default();
}
static CHECKING: AtomicBool = AtomicBool::new(false);

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

fn base_url() -> String {
    let server = option_env!("RENDEZVOUS_SERVER").unwrap_or("support.elvs.it");
    let host = server.split(':').next().unwrap_or(server).trim();
    format!("https://{}/elvs/techniker/", host)
}

/// POST to the server in a thread of its own (post_request_sync starts its own runtime).
fn request(action: &'static str, body: serde_json::Value) -> Result<serde_json::Value, String> {
    let url = format!("{}{}", base_url(), action);
    let handle = std::thread::spawn(move || {
        crate::post_request_sync(url, body.to_string(), "Content-Type: application/json")
    });
    let text = match handle.join() {
        Ok(Ok(text)) => text,
        Ok(Err(e)) => return Err(format!("Server nicht erreichbar: {}", e)),
        Err(_) => return Err("Server nicht erreichbar".to_owned()),
    };
    serde_json::from_str::<serde_json::Value>(&text)
        .map_err(|_| "Unerwartete Antwort vom Server".to_owned())
}

fn answer_error(v: &serde_json::Value) -> String {
    v.get("fehler")
        .and_then(|f| f.as_str())
        .unwrap_or("Abgelehnt")
        .to_owned()
}

fn token() -> String {
    LocalConfig::get_option(OPT_TOKEN)
}

fn is_permanent() -> bool {
    LocalConfig::get_option(OPT_PERMANENT) == "Y"
}

fn clear() {
    for k in [OPT_TOKEN, OPT_PERMANENT, OPT_LAST, OPT_TECH, OPT_NAME] {
        LocalConfig::set_option(k.to_owned(), "".to_owned());
    }
    if let Ok(mut c) = LAST_CHECK.lock() {
        *c = None;
    }
}

fn last_activity() -> u64 {
    LocalConfig::get_option(OPT_LAST)
        .parse::<u64>()
        .unwrap_or(0)
}

/// True while the full client (technician view) is unlocked on this computer.
pub fn is_active() -> bool {
    let token = token();
    if token.is_empty() {
        return false;
    }
    if !is_permanent() && now().saturating_sub(last_activity()) > IDLE_SECS {
        log::info!("elvs: Techniker-Modus nach 10 Minuten ohne Aktivität beendet");
        clear();
        return false;
    }
    check_in_background(token);
    true
}

/// The main window shows the customer view ("incoming only" layout).
pub fn customer_view() -> bool {
    !is_active()
}

fn check_in_background(token: String) {
    let due = match LAST_CHECK.lock() {
        Ok(c) => c.map_or(true, |t| t.elapsed().as_secs() >= CHECK_EVERY_SECS),
        Err(_) => false,
    };
    if !due || CHECKING.swap(true, Ordering::SeqCst) {
        return;
    }
    std::thread::spawn(move || {
        match request("pruefen", serde_json::json!({ "token": token })) {
            Ok(v) if v.get("ok").and_then(|o| o.as_bool()) == Some(true) => {
                if let Ok(mut c) = LAST_CHECK.lock() {
                    *c = Some(Instant::now());
                }
            }
            Ok(v) => {
                log::info!(
                    "elvs: Techniker-Freischaltung abgelehnt: {}",
                    answer_error(&v)
                );
                if LocalConfig::get_option(OPT_TOKEN) == token {
                    clear();
                }
            }
            Err(e) => {
                // Server not reachable: keep the unlock, try again later.
                log::warn!("elvs: Techniker-Freischaltung nicht geprüft: {}", e);
                if let Ok(mut c) = LAST_CHECK.lock() {
                    *c = Some(Instant::now());
                }
            }
        }
        CHECKING.store(false, Ordering::SeqCst);
    });
}

fn touch() {
    if token().is_empty() || is_permanent() {
        return;
    }
    let now = now();
    let last = last_activity();
    if now.saturating_sub(last) <= IDLE_SECS && now.saturating_sub(last) >= TOUCH_EVERY_SECS {
        LocalConfig::set_option(OPT_LAST.to_owned(), now.to_string());
    }
}

fn unlock(value: &str) {
    let args = serde_json::from_str::<serde_json::Value>(value).unwrap_or_default();
    let code: String = args
        .get("code")
        .and_then(|c| c.as_str())
        .unwrap_or_default()
        .chars()
        .filter(|c| c.is_ascii_digit())
        .collect();
    let name = args
        .get("name")
        .and_then(|c| c.as_str())
        .unwrap_or_default()
        .trim()
        .to_owned();
    let permanent = args.get("dauerhaft").and_then(|c| c.as_bool()) == Some(true);
    if let Ok(mut s) = STATUS.lock() {
        if s.busy {
            return;
        }
        s.busy = true;
        s.error.clear();
    }
    std::thread::spawn(move || {
        let body = serde_json::json!({
            "code": code,
            "name": name,
            "geraet": crate::ui_interface::get_id(),
            "dauerhaft": permanent,
        });
        let error = match request("freischalten", body) {
            Ok(v) => match v.get("token").and_then(|t| t.as_str()) {
                Some(t) if v.get("ok").and_then(|o| o.as_bool()) == Some(true) => {
                    LocalConfig::set_option(OPT_TOKEN.to_owned(), t.to_owned());
                    LocalConfig::set_option(
                        OPT_PERMANENT.to_owned(),
                        if permanent { "Y" } else { "" }.to_owned(),
                    );
                    LocalConfig::set_option(OPT_LAST.to_owned(), now().to_string());
                    LocalConfig::set_option(
                        OPT_TECH.to_owned(),
                        v.get("techniker")
                            .and_then(|t| t.as_str())
                            .unwrap_or_default()
                            .to_owned(),
                    );
                    LocalConfig::set_option(OPT_NAME.to_owned(), name);
                    if let Ok(mut c) = LAST_CHECK.lock() {
                        *c = Some(Instant::now());
                    }
                    log::info!(
                        "elvs: Techniker-Modus freigeschaltet (dauerhaft: {})",
                        permanent
                    );
                    String::new()
                }
                _ => answer_error(&v),
            },
            Err(e) => e,
        };
        if let Ok(mut s) = STATUS.lock() {
            s.busy = false;
            s.error = error;
        }
    });
}

fn lock() {
    let token = token();
    clear();
    if !token.is_empty() {
        std::thread::spawn(move || {
            let _ = request("entfernen", serde_json::json!({ "token": token }));
        });
    }
}

fn status() -> String {
    let (busy, error) = match STATUS.lock() {
        Ok(s) => (s.busy, s.error.clone()),
        Err(_) => (false, String::new()),
    };
    serde_json::json!({
        "aktiv": is_active(),
        "dauerhaft": is_permanent(),
        "techniker": LocalConfig::get_option(OPT_TECH),
        "name": LocalConfig::get_option(OPT_NAME),
        "busy": busy,
        "fehler": error,
    })
    .to_string()
}

fn list() -> String {
    let token = token();
    if token.is_empty() {
        return serde_json::json!({ "ok": false, "fehler": "Nicht freigeschaltet" }).to_string();
    }
    match request("liste", serde_json::json!({ "token": token })) {
        Ok(v) => v.to_string(),
        Err(e) => serde_json::json!({ "ok": false, "fehler": e }).to_string(),
    }
}

fn remove(id: &str) -> String {
    let token = token();
    match request("entfernen", serde_json::json!({ "token": token, "id": id })) {
        Ok(v) => v.to_string(),
        Err(e) => serde_json::json!({ "ok": false, "fehler": e }).to_string(),
    }
}

/// Keys for main_get_common. "elvs-tech-liste" and "elvs-tech-entfernen:<id>" do network
/// requests and must only be used on the asynchronous FFI path.
pub fn get_common(key: &str) -> Option<String> {
    match key {
        "elvs-tech-aktiv" => Some(is_active().to_string()),
        "elvs-tech-status" => Some(status()),
        "elvs-tech-liste" => Some(list()),
        _ => key.strip_prefix("elvs-tech-entfernen:").map(remove),
    }
}

/// Keys for main_set_common. Returns true when the key belongs to the Techniker-Modus.
pub fn set_common(key: &str, value: &str) -> bool {
    match key {
        "elvs-tech-freischalten" => unlock(value),
        "elvs-tech-sperren" => lock(),
        "elvs-tech-aktivitaet" => touch(),
        _ => return false,
    }
    true
}
