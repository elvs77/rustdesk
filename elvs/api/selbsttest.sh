#!/bin/bash
# elvs Selbsttest für den Fernwartungs-Server (ovhvm25, support.elvs.it)
#
# Prüft, ob alles da ist, läuft und unverändert ist:
#   - Prüfsummen (SHA-256): eigener hbbs, Geräte-Empfänger, Dienst-Datei, Compose-Dateien,
#     nginx-Konfiguration und öffentlicher Server-Schlüssel – gegen die Soll-Liste
#   - Docker: hbbs und hbbr laufen, festes Image (Digest), hbbs ist der eigene Build
#   - Ports 21114–21119, Dienste elvs-api, nginx, certbot.timer
#   - Zertifikat support.elvs.it (Restlaufzeit), WebSocket-Weg (101), Geräte-Empfänger
#   - Plattenplatz
# Ergebnis: Tabelle auf dem Bildschirm und /var/lib/elvs-selbsttest/ergebnis.json
# (die Büro-App zeigt es unter Fernwartung-Geräte an).
#
#   sudo /srv/rustdesk/selbsttest/selbsttest.sh            # prüfen
#   sudo /srv/rustdesk/selbsttest/selbsttest.sh --abnahme  # Konfiguration als neuen Soll-Stand übernehmen
#                                                          # (nur nach einer gewollten Änderung!)
set -u
VERSION="1.0"
DIR=/srv/rustdesk/selbsttest
SOLL_FEST=$DIR/soll-fest.sha256          # Programme: Werte aus dem Release, nie automatisch geändert
SOLL_KONF=$DIR/soll-konfiguration.sha256 # Konfiguration: mit --abnahme festgeschrieben
OUT=/var/lib/elvs-selbsttest
IMAGE_DIGEST="sha256:8ecdab65deb7c84652a626380e31d11a8f1fbafd97916d57f95c20628f943c00"
KEY_ENDE="Kxi54="
KONF_DATEIEN="/srv/rustdesk/compose.yml /srv/rustdesk/compose.override.yml /etc/nginx/sites-available/rustdesk.conf /srv/rustdesk/data/id_ed25519.pub"

[ "$(id -u)" = 0 ] || { echo "Bitte mit sudo starten."; exit 2; }
mkdir -p "$OUT"; chmod 755 "$OUT"

if [ "${1:-}" = "--abnahme" ]; then
  sha256sum $KONF_DATEIEN > "$SOLL_KONF.neu" && mv "$SOLL_KONF.neu" "$SOLL_KONF" && chmod 644 "$SOLL_KONF"
  echo "Soll-Stand der Konfiguration übernommen:"; cat "$SOLL_KONF"; exit 0
fi

ERG=()   # Zeilen: status|id|titel|text
add(){ ERG+=("$1|$2|$3|$4"); }

# 1. Programme (fest)
if [ -f "$SOLL_FEST" ]; then
  while read -r soll datei; do
    [ -z "$soll" ] && continue
    ist=$(sha256sum "$datei" 2>/dev/null | cut -d' ' -f1)
    if [ -z "$ist" ]; then add fehler "datei:$datei" "Programm $(basename "$datei")" "fehlt"
    elif [ "$ist" = "$soll" ]; then add ok "datei:$datei" "Programm $(basename "$datei")" "Prüfsumme stimmt"
    else add fehler "datei:$datei" "Programm $(basename "$datei")" "Prüfsumme weicht ab – Datei wurde verändert"; fi
  done < "$SOLL_FEST"
else add fehler soll-fest "Soll-Liste Programme" "$SOLL_FEST fehlt"; fi

# 2. Konfiguration (abgenommen)
if [ -f "$SOLL_KONF" ]; then
  while read -r soll datei; do
    [ -z "$soll" ] && continue
    ist=$(sha256sum "$datei" 2>/dev/null | cut -d' ' -f1)
    if [ -z "$ist" ]; then add fehler "konf:$datei" "Konfiguration $(basename "$datei")" "fehlt"
    elif [ "$ist" = "$soll" ]; then add ok "konf:$datei" "Konfiguration $(basename "$datei")" "unverändert seit der Abnahme"
    else add warnung "konf:$datei" "Konfiguration $(basename "$datei")" "geändert seit der Abnahme – gewollt? Dann: selbsttest.sh --abnahme"; fi
  done < "$SOLL_KONF"
else add warnung soll-konf "Soll-Stand Konfiguration" "noch keine Abnahme – einmal: sudo $DIR/selbsttest.sh --abnahme"; fi

# 3. Server-Schlüssel
pub=$(cat /srv/rustdesk/data/id_ed25519.pub 2>/dev/null)
if [ -z "$pub" ]; then add fehler schluessel "Server-Schlüssel" "id_ed25519.pub fehlt"
elif [ "${pub: -${#KEY_ENDE}}" = "$KEY_ENDE" ]; then add ok schluessel "Server-Schlüssel" "…$KEY_ENDE wie in den Clients"
else add fehler schluessel "Server-Schlüssel" "anderer Schlüssel als in den Clients – kein Client kann sich verbinden"; fi
[ -f /srv/rustdesk/data/id_ed25519 ] && [ "$(stat -c %a /srv/rustdesk/data/id_ed25519)" -le 600 ] \
  && add ok schluessel-rechte "Privater Schlüssel" "vorhanden, nur für root lesbar" \
  || add warnung schluessel-rechte "Privater Schlüssel" "fehlt oder für andere lesbar"

# 4. Docker
for c in hbbs hbbr; do
  run=$(docker inspect -f '{{.State.Running}}' "$c" 2>/dev/null)
  img=$(docker inspect -f '{{.Image}}' "$c" 2>/dev/null)
  rep=$(docker image inspect -f '{{join .RepoDigests ","}}' "$img" 2>/dev/null)
  if [ "$run" != "true" ]; then add fehler "docker:$c" "Container $c" "läuft nicht"
  elif [[ "$rep" == *"$IMAGE_DIGEST"* ]]; then add ok "docker:$c" "Container $c" "läuft, festes Image"
  else add warnung "docker:$c" "Container $c" "läuft, aber nicht mit dem festgelegten Image"; fi
done
docker inspect -f '{{range .Mounts}}{{.Source}} {{end}}' hbbs 2>/dev/null | grep -q /srv/rustdesk/bin/hbbs-elvs \
  && add ok hbbs-eigen "Eigener hbbs" "eingehängt (Anmeldung über WebSocket)" \
  || add warnung hbbs-eigen "Eigener hbbs" "nicht eingehängt – läuft der freie hbbs (Rückweg)?"

# 5. Ports und Dienste
lst=$(ss -ltnuH 2>/dev/null)
for p in 21114 21115 21116 21117 21118 21119; do
  echo "$lst" | grep -qE "[:.]$p\b" && add ok "port:$p" "Port $p" "offen" || add fehler "port:$p" "Port $p" "nicht offen"
done
echo "$lst" | grep -E "^udp" | grep -qE "[:.]21116\b" && add ok "port:21116u" "Port 21116 UDP" "offen" || add fehler "port:21116u" "Port 21116 UDP" "nicht offen"
for s in elvs-api nginx certbot.timer; do
  systemctl is-active --quiet "$s" && add ok "dienst:$s" "Dienst $s" "aktiv" || add fehler "dienst:$s" "Dienst $s" "nicht aktiv"
done

# 6. Zertifikat, WebSocket, Geräte-Empfänger
cert=/etc/letsencrypt/live/support.elvs.it/cert.pem
if end=$(openssl x509 -enddate -noout -in "$cert" 2>/dev/null | cut -d= -f2); then
  tage=$(( ( $(date -d "$end" +%s) - $(date +%s) ) / 86400 ))
  if [ "$tage" -ge 14 ]; then add ok zertifikat "Zertifikat support.elvs.it" "noch $tage Tage gültig"
  elif [ "$tage" -ge 0 ]; then add warnung zertifikat "Zertifikat support.elvs.it" "nur noch $tage Tage gültig – Verlängerung prüfen"
  else add fehler zertifikat "Zertifikat support.elvs.it" "abgelaufen"; fi
else add fehler zertifikat "Zertifikat support.elvs.it" "nicht gefunden"; fi
ws=$(curl -s -o /dev/null --max-time 5 -w "%{http_code}" --resolve support.elvs.it:443:127.0.0.1 \
  -H "Connection: Upgrade" -H "Upgrade: websocket" -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: SGVsbG8sIHdvcmxkIQ==" \
  https://support.elvs.it/ws/id)
[ "$ws" = "101" ] && add ok websocket "WebSocket-Weg (443)" "antwortet (101)" || add fehler websocket "WebSocket-Weg (443)" "antwortet nicht richtig ($ws)"
curl -s --max-time 5 http://127.0.0.1:21114/api/elvs/ok | grep -q '"ok":true' \
  && add ok api "Geräte-Empfänger" "antwortet" || add fehler api "Geräte-Empfänger" "antwortet nicht"

# 7. Plattenplatz
frei=$(df --output=pcent / | tail -1 | tr -dc 0-9); frei=$((100 - frei))
[ "$frei" -ge 15 ] && add ok platte "Plattenplatz" "$frei % frei" || add warnung platte "Plattenplatz" "nur $frei % frei"

# Ausgabe
gesamt=ok
for z in "${ERG[@]}"; do s=${z%%|*}; [ "$s" = fehler ] && gesamt=fehler; [ "$s" = warnung ] && [ "$gesamt" = ok ] && gesamt=warnung; done
am=$(date -u +%Y-%m-%dT%H:%M:%SZ)
{
  printf '{"version":"%s","am":"%s","gesamt":"%s","pruefungen":[' "$VERSION" "$am" "$gesamt"
  sep=""
  for z in "${ERG[@]}"; do IFS='|' read -r s i t x <<< "$z"
    printf '%s{"status":"%s","id":"%s","titel":"%s","text":"%s"}' "$sep" "$s" "${i//\"/}" "${t//\"/}" "${x//\"/}"; sep=","; done
  printf ']}\n'
} > "$OUT/ergebnis.json.neu" && mv "$OUT/ergebnis.json.neu" "$OUT/ergebnis.json" && chmod 644 "$OUT/ergebnis.json"
for z in "${ERG[@]}"; do IFS='|' read -r s i t x <<< "$z"
  case $s in ok) m="  OK    ";; warnung) m="  WARN  ";; *) m="  FEHLER";; esac
  printf '%s  %-34s %s\n' "$m" "$t" "$x"; done
echo; echo "Gesamt: $gesamt ($(date '+%d.%m.%Y %H:%M'))"
[ "$gesamt" = fehler ] && exit 1 || exit 0
