//! elvs (build 15): a click approval outlives a logoff, a user switch and a restart.
//!
//! When the customer clicks "Akzeptieren" for a remote-desktop connection, the approval is
//! remembered for that technician's session (peer id + the session id the controller chose at
//! random for this window and sends again on every reconnect, over the encrypted channel).
//! While the connection lasts the approval is renewed; once it drops it stays valid for
//! `GRANT_MINUTES`. A reconnect of the same window within that time is let in without a new
//! click, also at the Windows login screen where nobody could click. A new window, another
//! technician or a later attempt needs a click again. The customer ends the approval at once
//! with "Trennen" in the connection window.
//!
//! The approvals are kept in a small file next to the server's config, so they survive the
//! server process being restarted on a session change and the machine being rebooted. Only a
//! hash of the session id is written.

use hbb_common::{config::Config, log};
use serde_derive::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::HashMap,
    sync::Mutex,
    time::{Instant, SystemTime, UNIX_EPOCH},
};

/// How long an approval stays valid after the connection dropped.
pub const GRANT_MINUTES: u64 = 15;
const GRANT_SECS: u64 = GRANT_MINUTES * 60;
/// How often a running connection renews its approval on disk.
const RENEW_EVERY_SECS: u64 = 30;
const FILE_NAME: &str = "elvs-freigabe.json";

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
struct Grant {
    peer_id: String,
    session: String,
    expires: u64,
}

lazy_static::lazy_static! {
    static ref LOCK: Mutex<()> = Mutex::new(());
    static ref LAST_RENEW: Mutex<HashMap<String, Instant>> = Default::default();
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

fn session_hash(peer_id: &str, session_id: u64) -> String {
    let mut h = Sha256::new();
    h.update(b"elvs-freigabe\0");
    h.update(peer_id.as_bytes());
    h.update(b"\0");
    h.update(session_id.to_le_bytes());
    hex::encode(h.finalize())
}

fn load() -> Vec<Grant> {
    let path = Config::path(FILE_NAME);
    match std::fs::read_to_string(&path) {
        Ok(s) => serde_json::from_str::<Vec<Grant>>(&s).unwrap_or_default(),
        Err(_) => Vec::new(),
    }
}

fn store(grants: &[Grant]) {
    let path = Config::path(FILE_NAME);
    if grants.is_empty() {
        let _ = std::fs::remove_file(&path);
        return;
    }
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    match serde_json::to_string(grants) {
        Ok(s) => {
            if let Err(e) = std::fs::write(&path, s) {
                log::error!("elvs: Freigabe konnte nicht gespeichert werden: {}", e);
            }
        }
        Err(e) => log::error!("elvs: Freigabe konnte nicht geschrieben werden: {}", e),
    }
}

fn live(grants: Vec<Grant>) -> Vec<Grant> {
    let t = now();
    grants.into_iter().filter(|g| g.expires > t).collect()
}

fn valid_session(peer_id: &str, session_id: u64) -> bool {
    !peer_id.is_empty() && session_id != 0
}

/// The customer clicked "Akzeptieren": remember the approval for this window of this technician.
pub fn remember(peer_id: &str, session_id: u64) {
    if !valid_session(peer_id, session_id) {
        return;
    }
    let _l = LOCK.lock().unwrap();
    let hash = session_hash(peer_id, session_id);
    let mut grants = live(load());
    grants.retain(|g| g.session != hash);
    grants.push(Grant {
        peer_id: peer_id.to_owned(),
        session: hash.clone(),
        expires: now() + GRANT_SECS,
    });
    store(&grants);
    LAST_RENEW.lock().unwrap().insert(hash, Instant::now());
    log::info!(
        "elvs: Freigabe fuer {} gemerkt ({} Min. nach Trennung gueltig)",
        peer_id,
        GRANT_MINUTES
    );
}

/// Whether a reconnect of this window may come in without a click.
pub fn check(peer_id: &str, session_id: u64) -> bool {
    if !valid_session(peer_id, session_id) {
        return false;
    }
    let _l = LOCK.lock().unwrap();
    let hash = session_hash(peer_id, session_id);
    let all = load();
    let count = all.len();
    let grants = live(all);
    if grants.len() != count {
        store(&grants);
    }
    grants
        .iter()
        .any(|g| g.session == hash && g.peer_id == peer_id)
}

/// Called every second by a running connection: renews an existing approval now and then,
/// so it is still valid `GRANT_MINUTES` after the connection drops. Never creates one.
pub fn renew(peer_id: &str, session_id: u64) {
    if !valid_session(peer_id, session_id) {
        return;
    }
    let hash = session_hash(peer_id, session_id);
    {
        let mut last = LAST_RENEW.lock().unwrap();
        match last.get(&hash) {
            Some(t) if t.elapsed().as_secs() < RENEW_EVERY_SECS => return,
            _ => {}
        }
        last.insert(hash.clone(), Instant::now());
    }
    let _l = LOCK.lock().unwrap();
    let mut grants = live(load());
    let mut changed = false;
    for g in grants.iter_mut() {
        if g.session == hash && g.peer_id == peer_id {
            g.expires = now() + GRANT_SECS;
            changed = true;
        }
    }
    if changed {
        store(&grants);
    }
}

/// The customer ended the connection: the approval ends with it.
pub fn revoke(peer_id: &str, session_id: u64) {
    if !valid_session(peer_id, session_id) {
        return;
    }
    let _l = LOCK.lock().unwrap();
    let hash = session_hash(peer_id, session_id);
    let grants = load();
    let before = grants.len();
    let grants: Vec<Grant> = live(grants)
        .into_iter()
        .filter(|g| g.session != hash)
        .collect();
    if grants.len() != before {
        store(&grants);
    }
    LAST_RENEW.lock().unwrap().remove(&hash);
    log::info!("elvs: Freigabe fuer {} vom Kunden beendet", peer_id);
}

/// The customer ended all connections at once (e.g. "Alle trennen"): drop every approval.
#[allow(dead_code)]
pub fn revoke_all() {
    let _l = LOCK.lock().unwrap();
    store(&[]);
    LAST_RENEW.lock().unwrap().clear();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hash_depends_on_peer_and_session() {
        assert_ne!(session_hash("a", 1), session_hash("a", 2));
        assert_ne!(session_hash("a", 1), session_hash("b", 1));
        assert_eq!(session_hash("a", 1), session_hash("a", 1));
    }

    #[test]
    fn expired_grants_are_dropped() {
        let g = vec![
            Grant {
                peer_id: "a".into(),
                session: "x".into(),
                expires: 1,
            },
            Grant {
                peer_id: "b".into(),
                session: "y".into(),
                expires: now() + 60,
            },
        ];
        let l = live(g);
        assert_eq!(l.len(), 1);
        assert_eq!(l[0].peer_id, "b");
    }
}
