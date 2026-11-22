// elvs (16.2): Techniker-Modus. Without an unlock the client shows the customer view
// (logo, ID, password). The technician unlocks it with the code of his authenticator app;
// the code is checked by the own server (see src/elvs_tech.rs).

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:get/get.dart';

import '../../models/platform_model.dart';

class ElvsTech {
  static final RxBool active = RxBool(_read());

  /// 16.2.1: an accepted session runs on this computer (or ended < 10 minutes ago).
  static final RxBool session = RxBool(_readSession());
  static Timer? _timer;
  static int _lastTouch = 0;

  static bool _read() =>
      bind.mainGetCommonSync(key: 'elvs-tech-aktiv') == 'true';

  static bool _readSession() =>
      bind.mainGetCommonSync(key: 'elvs-tech-sitzung') == 'true';

  static bool customerView() => !_read();

  /// Settings limited to Techniker-Modus, language and About: customer view without session.
  static bool settingsLimited() => customerView() && !_readSession();

  /// Polls the state, so the 10-minute fallback also reaches an open window.
  static void start() {
    _timer ??=
        Timer.periodic(const Duration(seconds: 15), (_) => refresh());
  }

  static void refresh() {
    final v = _read();
    if (v != active.value) {
      active.value = v;
    }
    final w = _readSession();
    if (w != session.value) {
      session.value = w;
    }
  }

  /// Activity in the client window keeps a short unlock (customer PC) alive.
  static void touch() {
    if (!active.value) return;
    final now = DateTime.now().millisecondsSinceEpoch;
    if (now - _lastTouch < 20000) return;
    _lastTouch = now;
    bind.mainSetCommon(key: 'elvs-tech-aktivitaet', value: '');
  }

  /// Returns null on success, otherwise the error text.
  static Future<String?> unlock(String code, String name, bool permanent) async {
    await bind.mainSetCommon(
        key: 'elvs-tech-freischalten',
        value: jsonEncode({'code': code, 'name': name, 'dauerhaft': permanent}));
    for (var i = 0; i < 70; i++) {
      await Future.delayed(const Duration(milliseconds: 300));
      final s = _status();
      if (s['busy'] != true) {
        refresh();
        final e = (s['fehler'] ?? '').toString();
        if (e.isNotEmpty) return e;
        return s['aktiv'] == true ? null : 'Nicht freigeschaltet';
      }
    }
    return 'Keine Antwort vom Server';
  }

  static Future<void> lock() async {
    await bind.mainSetCommon(key: 'elvs-tech-sperren', value: '');
    refresh();
  }

  static Map<String, dynamic> _status() {
    try {
      final v = jsonDecode(bind.mainGetCommonSync(key: 'elvs-tech-status'));
      if (v is Map<String, dynamic>) return v;
    } catch (_) {}
    return {};
  }

  static Map<String, dynamic> status() => _status();
}

/// Content of the card "Techniker-Modus" in the settings.
class ElvsTechPanel extends StatefulWidget {
  const ElvsTechPanel({Key? key}) : super(key: key);

  @override
  State<ElvsTechPanel> createState() => _ElvsTechPanelState();
}

class _ElvsTechPanelState extends State<ElvsTechPanel> {
  Future<String>? _list;

  void _reloadList() {
    setState(() {
      _list = bind.mainGetCommon(key: 'elvs-tech-liste');
    });
  }

  @override
  Widget build(BuildContext context) {
    return Obx(() {
      final on = ElvsTech.active.value;
      final s = on ? ElvsTech.status() : <String, dynamic>{};
      if (on && _list == null) {
        _list = bind.mainGetCommon(key: 'elvs-tech-liste');
      }
      if (!on) _list = null;
      return Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(children: [
            Checkbox(
              value: on,
              onChanged: (v) async {
                if (v == true) {
                  await showElvsTechUnlockDialog(context);
                } else {
                  await ElvsTech.lock();
                }
              },
            ),
            const Expanded(child: Text('Techniker-Modus')),
          ]),
          if (on)
            Text(
              s['dauerhaft'] == true
                  ? 'Freigeschaltet für ${s['techniker'] ?? ''} – dieser Computer ist vertrauenswürdig.'
                  : 'Freigeschaltet für ${s['techniker'] ?? ''} – endet 10 Minuten nach der letzten Aktivität.',
              style: const TextStyle(fontSize: 13),
            ).marginOnly(left: 12, bottom: 8),
          if (on) ...[
            Row(children: [
              const Expanded(
                  child: Text('Vertrauenswürdige Computer',
                      style: TextStyle(fontWeight: FontWeight.w600))),
              IconButton(
                tooltip: 'Aktualisieren',
                icon: const Icon(Icons.refresh, size: 18),
                onPressed: _reloadList,
              ),
            ]).marginOnly(left: 12),
            FutureBuilder<String>(
              future: _list,
              builder: (context, snap) {
                if (!snap.hasData) {
                  return const Text('Wird geladen …').marginOnly(left: 12);
                }
                Map<String, dynamic> v = {};
                try {
                  v = jsonDecode(snap.data!);
                } catch (_) {}
                if (v['ok'] != true) {
                  return Text((v['fehler'] ?? 'Liste nicht verfügbar').toString())
                      .marginOnly(left: 12);
                }
                final items = (v['computer'] as List? ?? []);
                if (items.isEmpty) {
                  return const Text('Keine.').marginOnly(left: 12);
                }
                return ConstrainedBox(
                  // at most 5 rows visible, the rest by scrolling
                  constraints: const BoxConstraints(maxHeight: 5 * 48.0),
                  child: ListView(
                    shrinkWrap: true,
                    children: items.map<Widget>((c) {
                      final when = DateTime.fromMillisecondsSinceEpoch(
                          ((c['zuletzt'] ?? 0) as int) * 1000);
                      final dieser = c['dieser'] == true;
                      return ListTile(
                        dense: true,
                        title: Text('${c['name']}${dieser ? ' (dieser)' : ''}'),
                        subtitle: Text(
                            'ID ${c['geraet']} · zuletzt ${when.day.toString().padLeft(2, '0')}.${when.month.toString().padLeft(2, '0')}.${when.year}'),
                        trailing: IconButton(
                          tooltip: 'Entfernen',
                          icon: const Icon(Icons.delete_outline, size: 18),
                          onPressed: () async {
                            await bind.mainGetCommon(
                                key: 'elvs-tech-entfernen:${c['id']}');
                            if (dieser) {
                              await ElvsTech.lock();
                            } else {
                              _reloadList();
                            }
                          },
                        ),
                      );
                    }).toList(),
                  ),
                ).marginOnly(left: 4);
              },
            ),
          ],
        ],
      );
    });
  }
}

Future<void> showElvsTechUnlockDialog(BuildContext context) async {
  final code = TextEditingController();
  final name = TextEditingController(text: Platform.localHostname);
  var permanent = false;
  var busy = false;
  String? error;
  await showDialog(
    context: context,
    barrierDismissible: false,
    builder: (ctx) => StatefulBuilder(builder: (ctx, setState) {
      Future<void> submit() async {
        if (busy) return;
        setState(() {
          busy = true;
          error = null;
        });
        final e = await ElvsTech.unlock(code.text.trim(), name.text.trim(), permanent);
        if (e == null) {
          if (ctx.mounted) Navigator.of(ctx).pop();
          return;
        }
        setState(() {
          busy = false;
          error = e;
          code.clear();
        });
      }

      return AlertDialog(
        title: const Text('Techniker-Modus freischalten'),
        content: SizedBox(
          width: 340,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text('Code aus der Authenticator-App (6 Stellen):'),
              TextField(
                controller: code,
                autofocus: true,
                maxLength: 6,
                keyboardType: TextInputType.number,
                inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                onSubmitted: (_) => submit(),
              ),
              const Text('Name dieses Computers:'),
              TextField(controller: name, maxLength: 60),
              Row(children: [
                Checkbox(
                    value: permanent,
                    onChanged: (v) => setState(() => permanent = v == true)),
                const Expanded(
                    child: Text(
                        'Diesem Computer dauerhaft vertrauen (nur eigene Rechner)')),
              ]),
              if (error != null)
                Text(error!, style: const TextStyle(color: Colors.red)),
            ],
          ),
        ),
        actions: [
          TextButton(
              onPressed: busy ? null : () => Navigator.of(ctx).pop(),
              child: const Text('Abbrechen')),
          ElevatedButton(
              onPressed: busy ? null : submit,
              child: Text(busy ? 'Wird geprüft …' : 'Freischalten')),
        ],
      );
    }),
  );
}
