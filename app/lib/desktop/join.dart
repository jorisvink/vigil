import 'dart:async';
import 'dart:convert';

import 'package:flutter/services.dart';

import '../reliquary.dart';
import 'desk.dart';

class Join {
  Join(this.api, this.onStep);

  final String api;
  final void Function(String) onStep;

  static const _channel = MethodChannel('se.sanctorum.vigil/desktop');

  bool cancelled = false;

  static Future<Set<String>> words() async {
    final list = await _channel.invokeMethod<List<Object?>>('words');
    return {for (final w in list ?? const []) w as String};
  }

  void _at(String step) {
    if (cancelled) {
      throw DeskError('stopped', 'Stopped.');
    }

    onStep(step);
  }

  Future<T?> _invoke<T>(String method, [Object? args]) async {
    try {
      return await _channel.invokeMethod<T>(method, args);
    } on PlatformException catch (e) {
      throw DeskError(e.code, e.message ?? e.code);
    }
  }

  Future<void> run(List<String> typed) async {
    try {
      await _run(typed);
    } finally {
      await _invoke<void>('remoteStop');
    }
  }

  Future<void> _run(List<String> typed) async {
    _at('Checking the secret words');

    final raw = await _invoke<String>('route', typed.join(' '));
    if (raw == null) {
      throw DeskError(
        'words',
        'These secret words are not right, check them against the ones '
            'you were given.',
      );
    }

    final route = jsonDecode(raw) as Map<String, dynamic>;

    _at('Joining the flock at Reliquary');

    final hex = route['cosk'] as String;
    final cosk = [
      for (var i = 0; i < hex.length; i += 2)
        int.parse(hex.substring(i, i + 2), radix: 16),
    ];

    final joined = await _create(route['flock'] as String, cosk);

    _at('Setting up the tunnel to Covenant');

    await _invoke<void>('remoteStart', {
      'id': joined['cathedral_id'] as String,
      'secret': joined['cathedral_secret'] as String,
    });

    _at('Waiting for Covenant, this can take up to a minute');

    final until = DateTime.now().add(const Duration(minutes: 10));

    while (true) {
      if (cancelled) {
        throw DeskError('stopped', 'Stopped.');
      }

      final state = await _invoke<int>('remoteRun') ?? -1;
      if (state == 3) {
        break;
      }

      if (state == -1) {
        throw DeskError('remote', 'The tunnel to Covenant failed, start over.');
      }

      if (state == 2) {
        _at('Connected, receiving the configuration');
      }

      if (DateTime.now().isAfter(until)) {
        throw DeskError(
          'remote',
          'Covenant did not show up in time, ask for new secret words and '
              'start over.',
        );
      }
    }

    if (!await Desk.instance.remoteLink()) {
      throw DeskError('remote', 'Covenant sent no configuration, start over.');
    }
  }

  Future<Map<String, dynamic>> _create(String flock, List<int> cosk) async {
    try {
      return await reliquaryCreate(api, flock, cosk);
    } on ReliquaryError catch (e) {
      throw DeskError('reliquary', e.message);
    }
  }
}
