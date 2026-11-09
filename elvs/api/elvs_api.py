#!/usr/bin/env python3
"""elvs Geräte-Empfänger für den RustDesk-Server (support.elvs.it).

Die RustDesk-Clients melden sich – wie früher bei RustDesk Pro – von selbst an der
API-Adresse: /api/heartbeat (alle 15 s bis wenige Minuten), /api/sysinfo (Computername,
Betriebssystem, Prozessor, Arbeitsspeicher, angemeldeter Benutzer, Version) und
/api/audit/conn (wer sich wann verbunden hat). Dieser kleine Dienst nimmt das an und
merkt es sich je Geräte-ID. Mehr nicht: keine Anmeldung, kein Adressbuch, keine
Steuerung der Clients.

Datensparsam (1.1): Nur Geräte, die in der App einem Kunden oder als eigener Rechner
zugeordnet sind (die App schickt ihre IDs mit POST /api/elvs/behalten), bleiben stehen.
Alle anderen werden 48 Stunden nach ihrer ersten Meldung komplett gelöscht – samt
Verbindungen. Meldet sich so ein Rechner danach wieder, beginnt er von vorn.

Die Büro-App holt die Liste mit GET /api/elvs/geraete und dem Schlüssel aus der Datei
token (Kopfzeile "Authorization: Bearer <token>"). Zuordnen zu Kunden passiert nur in
der App.

Neue Clients (elvs-Build) kommen über nginx (https://support.elvs.it/api/...),
alte Pro-Clients direkt auf Port 21114 (http://support.elvs.it:21114) – so sieht man
auch, wer noch den alten Client hat.

Nur Python-Standardbibliothek. Start: python3 elvs_api.py --data /var/lib/elvs-api
"""
import argparse, hmac, ipaddress, json, os, re, secrets, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "1.1"
FRIST_STUNDEN = 48  # nicht zugeordnete Geräte fallen nach 48 h ganz heraus
MAX_BODY = 256 * 1024
MAX_GERAETE = 5000
MAX_VERBINDUNGEN = 30
ID_RE = re.compile(r"^[0-9A-Za-z_-]{1,40}$")
TEXT_FELDER = ("hostname", "os", "cpu", "memory", "username", "version", "uuid")


def jetzt():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def kurz(v, n=200):
    return str(v if v is not None else "")[:n]


class Ablage:
    """Geräte im Speicher, alle paar Sekunden atomar nach geraete.json geschrieben."""

    def __init__(self, ordner):
        self.ordner = ordner
        self.datei = os.path.join(ordner, "geraete.json")
        self.lock = threading.Lock()
        self.geaendert = False
        self.behalten = set()
        try:
            with open(self.datei, encoding="utf-8") as f:
                d = json.load(f)
                self.geraete = d.get("geraete", {})
                self.behalten = set(d.get("behalten", []))
        except FileNotFoundError:
            self.geraete = {}
        threading.Thread(target=self._schreiber, daemon=True).start()

    def _schreiber(self):
        n = 0
        while True:
            time.sleep(5)
            n += 1
            if n % 60 == 1:  # alle 5 Minuten
                self.aufraeumen()
            self.speichern()

    def aufraeumen(self, jetzt_ts=None):
        """Nicht zugeordnete Geräte 48 h nach der ersten Meldung löschen."""
        grenze = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime((jetzt_ts or time.time()) - FRIST_STUNDEN * 3600))
        with self.lock:
            weg = [i for i, g in self.geraete.items() if i not in self.behalten and g.get("erstmals", "") < grenze]
            for i in weg:
                del self.geraete[i]
            if weg:
                self.geaendert = True
            return len(weg)

    def behalten_setzen(self, ids):
        with self.lock:
            self.behalten = set(ids)
            self.geaendert = True

    def speichern(self):
        with self.lock:
            if not self.geaendert:
                return
            daten = json.dumps({"version": VERSION, "stand": jetzt(), "geraete": self.geraete, "behalten": sorted(self.behalten)}, ensure_ascii=False)
            self.geaendert = False
        tmp = self.datei + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(daten)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.datei)

    def geraet(self, gid):
        g = self.geraete.get(gid)
        if g is None:
            if len(self.geraete) >= MAX_GERAETE:
                return None
            g = self.geraete[gid] = {"id": gid, "erstmals": jetzt()}
        return g

    def melden(self, gid, ip, weg, felder=None, verbindung=None, sysinfo=False):
        with self.lock:
            g = self.geraet(gid)
            if g is None:
                return None
            g["zuletzt"] = jetzt()
            g["ip"] = ip
            g["weg"] = weg  # "neu" = über nginx/https (elvs-Client), "alt" = direkt Port 21114 (Pro-Client)
            if felder:
                for k in TEXT_FELDER:
                    if k in felder:
                        g[k] = kurz(felder[k])
            if sysinfo:
                g["sysinfo_am"] = jetzt()
            if verbindung:
                v = g.setdefault("verbindungen", [])
                v.insert(0, verbindung)
                del v[MAX_VERBINDUNGEN:]
            self.geaendert = True
            return dict(g)

    def liste(self):
        with self.lock:
            return [dict(g, behalten=g["id"] in self.behalten) for g in self.geraete.values()]


def client_ip(h):
    for kopf in ("X-Real-IP", "X-Forwarded-For"):
        roh = (h.headers.get(kopf) or "").split(",")[0].strip().lstrip("\\")
        try:
            return str(ipaddress.ip_address(roh))
        except ValueError:
            pass
    return h.client_address[0]


def ueber_nginx(h):
    # nginx auf demselben Rechner leitet https weiter; direkte Verbindungen kommen von außen
    return h.client_address[0] in ("127.0.0.1", "::1") and bool(h.headers.get("X-Real-IP") or h.headers.get("X-Forwarded-For"))


class Handler(BaseHTTPRequestHandler):
    server_version = "elvs-api/" + VERSION
    sys_version = ""

    def log_message(self, fmt, *args):  # kein Zugriffsprotokoll mit IP-Adressen
        pass

    def antwort(self, code, text, typ="application/json"):
        b = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", typ + "; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b)

    def body_json(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return None
        if n <= 0 or n > MAX_BODY:
            return None
        try:
            v = json.loads(self.rfile.read(n).decode("utf-8", "replace"))
        except (ValueError, UnicodeDecodeError):
            return None
        return v if isinstance(v, dict) else None

    def do_GET(self):
        if self.path.split("?")[0] == "/api/elvs/geraete":
            # Liste nur über https (nginx), nie über das unverschlüsselte Port 21114
            if not ueber_nginx(self):
                return self.antwort(403, '{"error":"Nur über https"}')
            tok = (self.headers.get("Authorization") or "")
            if not (tok.startswith("Bearer ") and hmac.compare_digest(tok[7:].strip(), self.server.token)):
                time.sleep(1)
                return self.antwort(401, '{"error":"Nicht berechtigt"}')
            return self.antwort(200, json.dumps({"version": VERSION, "stand": jetzt(), "geraete": self.server.ablage.liste()}, ensure_ascii=False))
        if self.path == "/api/elvs/ok":
            return self.antwort(200, '{"ok":true}')
        return self.antwort(404, '{"error":"Nicht unterstützt"}')

    def berechtigt(self):
        tok = (self.headers.get("Authorization") or "")
        ok = ueber_nginx(self) and tok.startswith("Bearer ") and hmac.compare_digest(tok[7:].strip(), self.server.token)
        if not ok:
            time.sleep(1)
        return ok

    def do_POST(self):
        pfad = self.path.split("?")[0]
        if pfad == "/api/elvs/behalten":
            if not self.berechtigt():
                return self.antwort(401, '{"error":"Nicht berechtigt"}')
            v = self.body_json()
            ids = (v or {}).get("ids")
            if not isinstance(ids, list) or len(ids) > MAX_GERAETE or not all(isinstance(i, str) and ID_RE.match(i) for i in ids):
                return self.antwort(400, '{"error":"Ungültige Liste"}')
            self.server.ablage.behalten_setzen(ids)
            return self.antwort(200, json.dumps({"ok": True, "behalten": len(ids), "geloescht": self.server.ablage.aufraeumen()}))
        weg = "neu" if ueber_nginx(self) else "alt"
        if pfad == "/api/sysinfo_ver":
            # Version unserer Ablage: gleich = Client muss unveränderte Daten nicht neu schicken
            return self.antwort(200, "1", "text/plain")
        v = self.body_json()
        gid = kurz((v or {}).get("id"), 60)
        if pfad in ("/api/heartbeat", "/api/sysinfo", "/api/audit/conn") and (v is None or not ID_RE.match(gid)):
            return self.antwort(400, '{"error":"Ungültige Meldung"}')
        ip = client_ip(self)
        a = self.server.ablage
        if pfad == "/api/heartbeat":
            felder = {"version": v["ver"]} if isinstance(v.get("ver"), (int, str)) and not str(v.get("ver")).isdigit() else None
            g = a.melden(gid, ip, weg, felder)
            if g is None:
                return self.antwort(200, "{}")
            # Noch keine Ausstattung bekannt: den Client bitten, sie zu schicken
            return self.antwort(200, '{"sysinfo":1}' if not g.get("sysinfo_am") else "{}")
        if pfad == "/api/sysinfo":
            a.melden(gid, ip, weg, v, sysinfo=True)
            return self.antwort(200, "SYSINFO_UPDATED", "text/plain")
        if pfad == "/api/audit/conn":
            peer = v.get("peer")
            if isinstance(peer, list) and peer:
                verb = {"zeit": jetzt(), "von_id": kurz(peer[0], 40), "von_name": kurz(peer[1] if len(peer) > 1 else "", 80), "art": kurz(v.get("type"), 10)}
                a.melden(gid, ip, weg, verbindung=verb)
            return self.antwort(200, "", "text/plain")
        if pfad.startswith("/api/audit/"):
            return self.antwort(200, "", "text/plain")
        return self.antwort(404, '{"error":"Nicht unterstützt"}')


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument("--data", default="/var/lib/elvs-api")
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=21114)
    a = p.parse_args(argv)
    os.makedirs(a.data, exist_ok=True)
    tf = os.path.join(a.data, "token")
    if not os.path.exists(tf):
        fd = os.open(tf, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(secrets.token_hex(32) + "\n")
    with open(tf) as f:
        token = f.read().strip()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    srv.daemon_threads = True
    srv.token = token
    srv.ablage = Ablage(a.data)
    print(f"elvs-api {VERSION} lauscht auf {a.host}:{a.port}, Daten in {a.data}", flush=True)
    try:
        srv.serve_forever()
    finally:
        srv.ablage.speichern()


if __name__ == "__main__":
    sys.exit(main())
