#!/bin/bash
# elvs Techniker-Pruefstelle auf ovhvm25 einrichten (Fernwartung 16.2).
# Aufruf als root im Ordner mit elvs-techniker.py:   bash installieren.sh
# Aendert nginx nur mit Sicherung; schlaegt "nginx -t" fehl, wird die alte Datei zurueckgelegt.
set -euo pipefail

ZIEL=/srv/rustdesk/techniker
NGINX=/etc/nginx/sites-available/rustdesk.conf
BENUTZER=elvs-techniker
JETZT=$(date +%F_%H%M%S)

[ "$(id -u)" = 0 ] || { echo "Bitte als root (sudo -i) starten."; exit 1; }
[ -f elvs-techniker.py ] || { echo "elvs-techniker.py nicht im aktuellen Ordner."; exit 1; }
[ -f "$NGINX" ] || { echo "$NGINX nicht gefunden – nichts geaendert."; exit 1; }
command -v python3 >/dev/null || { echo "python3 fehlt (apt install python3)."; exit 1; }

echo "1/5 Benutzer und Ordner"
id "$BENUTZER" >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin "$BENUTZER"
mkdir -p "$ZIEL/daten"
install -m 0755 elvs-techniker.py "$ZIEL/elvs-techniker.py"
chown root:root "$ZIEL" "$ZIEL/elvs-techniker.py"
chown "$BENUTZER:$BENUTZER" "$ZIEL/daten"
chmod 700 "$ZIEL/daten"

echo "2/5 Befehl elvs-techniker"
cat > /usr/local/bin/elvs-techniker <<EOF
#!/bin/sh
exec sudo -u $BENUTZER ELVS_TECHNIKER_DATEN=$ZIEL/daten python3 $ZIEL/elvs-techniker.py "\$@"
EOF
chmod 755 /usr/local/bin/elvs-techniker

echo "3/5 Dienst"
cat > /etc/systemd/system/elvs-techniker.service <<EOF
[Unit]
Description=elvs Techniker-Pruefstelle (Fernwartung)
After=network.target

[Service]
User=$BENUTZER
Environment=ELVS_TECHNIKER_DATEN=$ZIEL/daten
ExecStart=/usr/bin/python3 $ZIEL/elvs-techniker.py dienst
Restart=always
RestartSec=3
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=$ZIEL/daten
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now elvs-techniker
sleep 1
systemctl is-active --quiet elvs-techniker || { echo "Dienst startet nicht:"; journalctl -u elvs-techniker -n 20 --no-pager; exit 1; }

echo "4/5 nginx: Pfad /elvs/techniker/"
if grep -q "location /elvs/techniker/" "$NGINX"; then
  echo "   schon vorhanden, nginx bleibt unveraendert"
else
  cp -a "$NGINX" "$NGINX.vor-techniker-$JETZT"
  BLOCK='    location /elvs/techniker/ {\n        proxy_pass http://127.0.0.1:21120;\n        proxy_set_header X-Real-IP $remote_addr;\n        client_max_body_size 4k;\n    }\n'
  # vor die erste Zeile "location /ws/id" (im 443-Block) einsetzen
  awk -v block="$BLOCK" 'BEGIN{done=0} /location \/ws\/id/ && !done {printf "%s", block; done=1} {print}' \
    "$NGINX.vor-techniker-$JETZT" > "$NGINX"
  if ! grep -q "location /elvs/techniker/" "$NGINX"; then
    cp -a "$NGINX.vor-techniker-$JETZT" "$NGINX"; echo "   Stelle 'location /ws/id' nicht gefunden – nginx unveraendert."; exit 1
  fi
  if nginx -t 2>/dev/null; then
    systemctl reload nginx
    echo "   eingetragen, Sicherung: $NGINX.vor-techniker-$JETZT"
  else
    cp -a "$NGINX.vor-techniker-$JETZT" "$NGINX"; nginx -t || true
    echo "   nginx-Pruefung fehlgeschlagen – alte Datei wiederhergestellt."; exit 1
  fi
fi

echo "5/5 Probe"
sleep 1
if curl -fsS --max-time 5 https://support.elvs.it/elvs/techniker/ | grep -q elvs-techniker; then
  echo "   https://support.elvs.it/elvs/techniker/ antwortet. Fertig."
else
  echo "   Achtung: von aussen keine Antwort. Lokal:"; curl -sS --max-time 3 http://127.0.0.1:21120/elvs/techniker/ || true
fi
echo
echo "Naechster Schritt: elvs-techniker neu Elvis"
