# elvs Techniker-Prüfstelle (Fernwartung 16.2)

Der Client zeigt ohne Freischaltung nur die Kundenansicht (Logo, ID, Passwort, „Bereit“).
Der Techniker schaltet ihn unter **Einstellungen → Allgemein → Techniker-Modus** frei, mit dem
6-stelligen Code seiner Authenticator-App. Den Code prüft diese Prüfstelle auf ovhvm25. Das
Authenticator-Geheimnis bleibt auf dem Server; der Client bekommt nur ein Zufalls-Token, auf
dem Server liegt davon nur ein SHA-256-Hash.

- **Dauerhaft** (Haken „Diesem Computer dauerhaft vertrauen“, nur eigene Rechner): bleibt
  freigeschaltet und steht in der Liste „Vertrauenswürdige Computer“. Fällt nach 30 Tagen
  ohne Nutzung heraus.
- **Kunden-PC** (ohne Haken): endet 10 Minuten nach der letzten Aktivität im Client-Fenster,
  spätestens nach 12 Stunden. Die laufende Fernwartung wird dadurch nicht getrennt.
- **Während einer Fernwartungs-Sitzung** (ab 16.2.1): Solange eine angenommene Sitzung läuft und
  bis 10 Minuten danach, sind die Einstellungen des Clients vollständig offen – ohne Code. Der
  Technikbereich (Eingabefeld, Favoriten, Adressbuch) bleibt verdeckt, bis der Haken gesetzt wird.
- Falscher Code: 5 Versuche je IP in 10 Minuten, dann Sperre. Jeder Code gilt nur einmal.
- Keine Backup-Codes (Entscheidung 21.11.2026).

## Einrichten auf ovhvm25 (einmalig)

1. Ordner `elvs/techniker` (diese drei Dateien) auf den Server kopieren, z. B. nach `/root/techniker`.
2. Als root: `cd /root/techniker && bash installieren.sh`
   - legt den Systembenutzer `elvs-techniker` und den Dienst `elvs-techniker` an (Port 127.0.0.1:21120),
   - trägt in `/etc/nginx/sites-available/rustdesk.conf` den Pfad `/elvs/techniker/` ein
     (Sicherung `rustdesk.conf.vor-techniker-<Zeit>`; schlägt `nginx -t` fehl, wird zurückgestellt),
   - prüft `https://support.elvs.it/elvs/techniker/`.
3. Ersten Techniker anlegen: `elvs-techniker neu Elvis` – QR-Code mit der Authenticator-App
   scannen (für den QR-Code vorher `apt install qrencode`, sonst Schlüssel abtippen) und zur
   Probe einen Code eingeben.

## Betrieb

| Aufgabe | Befehl |
| --- | --- |
| Status | `systemctl status elvs-techniker` · `journalctl -u elvs-techniker -n 50` |
| Techniker und Computer anzeigen | `elvs-techniker liste` |
| Neuer Techniker (braucht den Code des ersten Technikers) | `elvs-techniker neu NAME` |
| Techniker entfernen | `elvs-techniker entfernen NAME` |
| Einen Computer entfernen | `elvs-techniker computer-entfernen ID` (ID aus `liste`) |
| Daten | `/srv/rustdesk/techniker/daten/` (techniker.json, geraete.json) – mit sichern! |

## Zurück

`systemctl disable --now elvs-techniker`, in nginx die Sicherung `rustdesk.conf.vor-techniker-*`
zurückkopieren, `nginx -t && systemctl reload nginx`. Ohne Prüfstelle lässt sich der Client nicht
freischalten (Kundenansicht bleibt); bereits dauerhaft freigeschaltete Rechner bleiben frei, solange
der Server nicht antwortet.
