#!/usr/bin/env python3
"""elvs Techniker-Pruefstelle (Fernwartung 16.2).

Prueft den 6-stelligen Authenticator-Code eines Technikers und gibt dem Client ein
zufaelliges Freischalt-Token zurueck. Das Authenticator-Geheimnis verlaesst nie diesen
Server. Gespeichert wird vom Token nur ein SHA-256-Hash.

Nur Python-Standardbibliothek. Laeuft hinter nginx auf 127.0.0.1:21120, Pfad /elvs/techniker/.

Befehle (als root):
  elvs-techniker.py dienst                 Pruefstelle starten (systemd)
  elvs-techniker.py neu NAME               Techniker anlegen (ab dem zweiten nur mit Code des ersten)
  elvs-techniker.py liste                  Techniker und vertrauenswuerdige Computer anzeigen
  elvs-techniker.py entfernen NAME         Techniker samt seinen Computern entfernen
  elvs-techniker.py computer-entfernen ID  einen Computer entfernen (ID aus "liste")
"""

import base64
import hashlib
import hmac
import json
import os
import secrets
import shutil
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DATEN = os.environ.get("ELVS_TECHNIKER_DATEN", "/srv/rustdesk/techniker/daten")
PORT = int(os.environ.get("ELVS_TECHNIKER_PORT", "21120"))
PFAD = "/elvs/techniker/"

DAUERHAFT_INAKTIV = 30 * 24 * 3600   # vertrauenswuerdiger Computer faellt nach 30 Tagen ohne Nutzung raus
KURZ_INAKTIV = 30 * 60               # Kunden-PC: Token verfaellt 30 Min. nach der letzten Pruefung
KURZ_HOECHSTENS = 12 * 3600          # Kunden-PC: spaetestens nach 12 Std.
FEHLER_JE_IP = 5                     # falsche Codes je IP in 10 Min.
FEHLER_GESAMT = 30                   # falsche Codes insgesamt in 10 Min.
FEHLER_FENSTER = 600

LOCK = threading.Lock()
FEHLER = {}  # ip -> [zeitpunkte]


# --- Speicher -------------------------------------------------------------------------------

def _pfad(name):
    return os.path.join(DATEN, name)


def laden(name):
    try:
        with open(_pfad(name), "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def speichern(name, daten):
    os.makedirs(DATEN, mode=0o700, exist_ok=True)
    tmp = _pfad(name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(daten, f, ensure_ascii=False, indent=1)
    os.chmod(tmp, 0o600)
    os.replace(tmp, _pfad(name))


# --- TOTP (RFC 6238, SHA-1, 30 s, 6 Stellen) ------------------------------------------------

def totp(geheimnis_b32, schritt):
    key = base64.b32decode(geheimnis_b32.upper() + "=" * (-len(geheimnis_b32) % 8))
    digest = hmac.new(key, struct.pack(">Q", schritt), hashlib.sha1).digest()
    off = digest[-1] & 0x0F
    zahl = (struct.unpack(">I", digest[off:off + 4])[0] & 0x7FFFFFFF) % 1000000
    return "%06d" % zahl


def code_pruefen(tech, code, jetzt=None):
    """Gibt den benutzten Zeitschritt zurueck oder None. Erlaubt +-1 Schritt, keine Wiederholung."""
    if not (isinstance(code, str) and len(code) == 6 and code.isdigit()):
        return None
    schritt = int((jetzt or time.time()) // 30)
    for s in (schritt - 1, schritt, schritt + 1):
        if hmac.compare_digest(totp(tech["geheimnis"], s), code):
            if s <= tech.get("letzter_schritt", -1):
                return None
            return s
    return None


def techniker_zum_code(techniker, code):
    for name, tech in techniker.items():
        s = code_pruefen(tech, code)
        if s is not None:
            tech["letzter_schritt"] = s
            return name
    return None


# --- Geraete --------------------------------------------------------------------------------

def token_hash(token):
    return hashlib.sha256(("elvs-techniker\0" + token).encode()).hexdigest()


def aufraeumen(geraete, jetzt):
    weg = []
    for h, g in geraete.items():
        if g.get("dauerhaft"):
            if jetzt - g.get("zuletzt", 0) > DAUERHAFT_INAKTIV:
                weg.append(h)
        elif jetzt - g.get("zuletzt", 0) > KURZ_INAKTIV or jetzt - g.get("erstellt", 0) > KURZ_HOECHSTENS:
            weg.append(h)
    for h in weg:
        del geraete[h]
    return bool(weg)


def zu_viele_fehler(ip, jetzt):
    for k in list(FEHLER):
        FEHLER[k] = [t for t in FEHLER[k] if jetzt - t < FEHLER_FENSTER]
        if not FEHLER[k]:
            del FEHLER[k]
    gesamt = sum(len(v) for v in FEHLER.values())
    return len(FEHLER.get(ip, [])) >= FEHLER_JE_IP or gesamt >= FEHLER_GESAMT


def text(v, laenge):
    return str(v or "").strip()[:laenge]


# --- Anfragen -------------------------------------------------------------------------------

def freischalten(daten, ip):
    jetzt = int(time.time())
    with LOCK:
        if zu_viele_fehler(ip, jetzt):
            return 429, {"ok": False, "fehler": "Zu viele falsche Codes. Bitte 10 Minuten warten."}
        techniker = laden("techniker.json")
        name = techniker_zum_code(techniker, text(daten.get("code"), 6))
        if not name:
            FEHLER.setdefault(ip, []).append(jetzt)
            return 403, {"ok": False, "fehler": "Der Code stimmt nicht oder wurde schon benutzt. Bitte den naechsten Code nehmen."}
        speichern("techniker.json", techniker)
        geraete = laden("geraete.json")
        aufraeumen(geraete, jetzt)
        token = secrets.token_urlsafe(32)
        geraete[token_hash(token)] = {
            "techniker": name,
            "name": text(daten.get("name"), 60) or "Computer",
            "geraet": text(daten.get("geraet"), 40),
            "dauerhaft": bool(daten.get("dauerhaft")),
            "erstellt": jetzt,
            "zuletzt": jetzt,
        }
        speichern("geraete.json", geraete)
        return 200, {"ok": True, "token": token, "techniker": name}


def _eintrag(daten, jetzt):
    geraete = laden("geraete.json")
    geaendert = aufraeumen(geraete, jetzt)
    h = token_hash(text(daten.get("token"), 100))
    return geraete, h, geraete.get(h), geaendert


def pruefen(daten, ip):
    jetzt = int(time.time())
    with LOCK:
        geraete, h, g, geaendert = _eintrag(daten, jetzt)
        if not g:
            if geaendert:
                speichern("geraete.json", geraete)
            return 403, {"ok": False, "fehler": "Freischaltung nicht mehr gueltig."}
        g["zuletzt"] = jetzt
        speichern("geraete.json", geraete)
        return 200, {"ok": True, "techniker": g["techniker"], "name": g["name"], "dauerhaft": g["dauerhaft"]}


def liste(daten, ip):
    jetzt = int(time.time())
    with LOCK:
        geraete, h, g, geaendert = _eintrag(daten, jetzt)
        if geaendert:
            speichern("geraete.json", geraete)
        if not g:
            return 403, {"ok": False, "fehler": "Freischaltung nicht mehr gueltig."}
        computer = [
            {"id": k[:12], "name": v["name"], "geraet": v.get("geraet", ""), "zuletzt": v["zuletzt"], "dieser": k == h}
            for k, v in geraete.items()
            if v["techniker"] == g["techniker"] and v.get("dauerhaft")
        ]
        computer.sort(key=lambda c: -c["zuletzt"])
        return 200, {"ok": True, "computer": computer}


def entfernen(daten, ip):
    jetzt = int(time.time())
    with LOCK:
        geraete, h, g, _ = _eintrag(daten, jetzt)
        if not g:
            return 403, {"ok": False, "fehler": "Freischaltung nicht mehr gueltig."}
        ziel = text(daten.get("id"), 12) or h[:12]
        treffer = [k for k, v in geraete.items() if k.startswith(ziel) and v["techniker"] == g["techniker"]]
        for k in treffer:
            del geraete[k]
        speichern("geraete.json", geraete)
        return 200, {"ok": True, "entfernt": len(treffer)}


AKTIONEN = {"freischalten": freischalten, "pruefen": pruefen, "liste": liste, "entfernen": entfernen}


class Handler(BaseHTTPRequestHandler):
    server_version = "elvs-techniker"
    sys_version = ""

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.headers.get("X-Real-IP", "-"), fmt % args))

    def antwort(self, code, daten):
        roh = json.dumps(daten, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(roh)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(roh)

    def do_GET(self):
        if self.path.rstrip("/") == PFAD.rstrip("/"):
            return self.antwort(200, {"ok": True, "dienst": "elvs-techniker"})
        self.antwort(404, {"ok": False})

    def do_POST(self):
        aktion = self.path[len(PFAD):].strip("/") if self.path.startswith(PFAD) else ""
        f = AKTIONEN.get(aktion)
        if not f:
            return self.antwort(404, {"ok": False})
        try:
            laenge = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            laenge = 0
        if laenge <= 0 or laenge > 4096:
            return self.antwort(400, {"ok": False})
        try:
            daten = json.loads(self.rfile.read(laenge).decode("utf-8"))
            if not isinstance(daten, dict):
                raise ValueError
        except (ValueError, UnicodeDecodeError):
            return self.antwort(400, {"ok": False})
        ip = self.headers.get("X-Real-IP") or self.client_address[0]
        try:
            code, inhalt = f(daten, ip)
        except Exception as e:  # nie Details nach aussen
            sys.stderr.write("Fehler bei %s: %r\n" % (aktion, e))
            code, inhalt = 500, {"ok": False, "fehler": "Interner Fehler"}
        self.antwort(code, inhalt)


# --- Befehle --------------------------------------------------------------------------------

def neu(name):
    name = name.strip()
    if not name:
        sys.exit("Name fehlt.")
    techniker = laden("techniker.json")
    if name in techniker:
        sys.exit("Den Techniker %s gibt es schon." % name)
    if techniker:
        erster = next(n for n, t in techniker.items() if t.get("erster")) if any(
            t.get("erster") for t in techniker.values()) else next(iter(techniker))
        code = input("Aktuellen Code von %s eingeben: " % erster).strip()
        s = code_pruefen(techniker[erster], code)
        if s is None:
            sys.exit("Code falsch. Nichts angelegt.")
        techniker[erster]["letzter_schritt"] = s
    geheimnis = base64.b32encode(secrets.token_bytes(20)).decode().rstrip("=")
    techniker[name] = {"geheimnis": geheimnis, "erster": not techniker, "angelegt": int(time.time())}
    uri = "otpauth://totp/elvs%%20Fernwartung:%s?secret=%s&issuer=elvs%%20Fernwartung&digits=6&period=30" % (
        name.replace(" ", "%20"), geheimnis)
    print("\nIn der Authenticator-App neues Konto hinzufuegen:\n")
    if shutil.which("qrencode"):
        subprocess.run(["qrencode", "-t", "ansiutf8", uri])
    else:
        print("(Fuer einen QR-Code vorher: apt install qrencode)")
    print("Oder Schluessel von Hand eingeben (zeitbasiert, 6 Stellen):")
    print("  Konto:     elvs Fernwartung (%s)" % name)
    print("  Schluessel: " + " ".join(geheimnis[i:i + 4] for i in range(0, len(geheimnis), 4)))
    code = input("\nZur Probe den Code aus der App eingeben: ").strip()
    s = code_pruefen(techniker[name], code)
    if s is None:
        sys.exit("Code passt nicht. Nichts gespeichert, bitte noch einmal.")
    techniker[name]["letzter_schritt"] = s
    speichern("techniker.json", techniker)
    print("Techniker %s angelegt." % name)


def zeige_liste():
    techniker = laden("techniker.json")
    geraete = laden("geraete.json")
    if aufraeumen(geraete, int(time.time())):
        speichern("geraete.json", geraete)
    if not techniker:
        print("Noch kein Techniker angelegt.")
    for name, t in techniker.items():
        print("%s%s" % (name, " (erster)" if t.get("erster") else ""))
        for h, g in sorted(geraete.items(), key=lambda x: -x[1]["zuletzt"]):
            if g["techniker"] == name:
                print("   %s  %-30s %-12s %s  %s" % (
                    h[:12], g["name"], g.get("geraet", ""), "dauerhaft" if g["dauerhaft"] else "Kunden-PC",
                    time.strftime("%d.%m.%Y %H:%M", time.localtime(g["zuletzt"]))))


def entferne_techniker(name):
    techniker = laden("techniker.json")
    if name not in techniker:
        sys.exit("Unbekannt: %s" % name)
    del techniker[name]
    geraete = {h: g for h, g in laden("geraete.json").items() if g["techniker"] != name}
    speichern("techniker.json", techniker)
    speichern("geraete.json", geraete)
    print("Entfernt: %s" % name)


def entferne_computer(ziel):
    geraete = laden("geraete.json")
    treffer = [h for h in geraete if ziel and h.startswith(ziel)]
    for h in treffer:
        del geraete[h]
    speichern("geraete.json", geraete)
    print("Entfernt: %d" % len(treffer))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return
    befehl = argv[1]
    if befehl == "dienst":
        os.makedirs(DATEN, mode=0o700, exist_ok=True)
        ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
    elif befehl == "neu" and len(argv) > 2:
        neu(" ".join(argv[2:]))
    elif befehl == "liste":
        zeige_liste()
    elif befehl == "entfernen" and len(argv) > 2:
        entferne_techniker(" ".join(argv[2:]))
    elif befehl == "computer-entfernen" and len(argv) > 2:
        entferne_computer(argv[2])
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv)
