import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';

import '../app.dart';
import '../theme.dart';
import '../vigil.dart';
import '../widgets/buttons.dart';
import '../widgets/ui.dart';
import 'import.dart';

const _reliquary = 'https://vessel.reliquary.se/v1';

const _wordCount = 33;

class JoinPage extends StatefulWidget {
  const JoinPage({super.key});

  @override
  State<JoinPage> createState() => _JoinPageState();
}

class _JoinPageState extends State<JoinPage> {
  final _words = TextEditingController();
  final _api = TextEditingController(text: _reliquary);

  Set<String> _list = const {};
  List<String> _sorted = const [];
  bool _running = false;
  bool _cancelled = false;
  String? _step;
  String? _problem;
  bool _advanced = false;

  @override
  void initState() {
    super.initState();
    _words.addListener(() => setState(() {}));

    Vigil.instance.remoteWordList().then((list) {
      if (mounted) {
        setState(() {
          _list = list.toSet();
          _sorted = [...list]..sort();
        });
      }
    }, onError: (_) {});
  }

  @override
  void dispose() {
    _cancelled = true;
    _words.clear();
    _words.dispose();
    _api.dispose();
    Vigil.instance.remoteStop().ignore();
    super.dispose();
  }

  List<String> get _typed => _words.text
      .toLowerCase()
      .split(RegExp(r'\s+'))
      .where((w) => w.isNotEmpty)
      .toList();

  List<String> get _unknown => [
    for (final w in _typed)
      if (_list.isNotEmpty && !_list.contains(w)) w,
  ];

  List<String> get _suggestions {
    final text = _words.text;
    if (text.isEmpty || text.endsWith(' ') || text.endsWith('\n')) {
      return const [];
    }

    final last = _typed.isEmpty ? '' : _typed.last;
    if (last.length < 2 || _list.contains(last)) {
      return const [];
    }

    return _sorted.where((w) => w.startsWith(last)).take(4).toList();
  }

  void _complete(String word) {
    final text = _words.text;
    final at = text.lastIndexOf(RegExp(r'\s')) + 1;

    _words.text = '${text.substring(0, at)}$word ';
    _words.selection = TextSelection.collapsed(offset: _words.text.length);
  }

  Uri? _endpoint(String path) {
    final api = _api.text.trim().replaceAll(RegExp(r'/+$'), '');
    final uri = Uri.tryParse('$api/$path');

    if (uri == null ||
        (uri.scheme != 'https' &&
            !(uri.scheme == 'http' && uri.host == '127.0.0.1'))) {
      return null;
    }

    return uri;
  }

  Future<Map<String, dynamic>> _create(String flock, List<int> cosk) async {
    final uri = _endpoint('device/$flock/create');
    if (uri == null) {
      throw VigilError('Reliquary must be an https address.');
    }

    final client = HttpClient()
      ..connectionTimeout = const Duration(seconds: 15);

    try {
      final request = await client.postUrl(uri);
      request.headers.contentType = ContentType.binary;
      request.contentLength = cosk.length;
      request.add(cosk);

      final response = await request.close().timeout(
        const Duration(seconds: 30),
      );
      final body = await response.transform(utf8.decoder).join();

      if (response.statusCode != 200) {
        throw VigilError(
          'Reliquary did not let us join (${response.statusCode}).',
        );
      }

      final json = jsonDecode(body) as Map<String, dynamic>;
      final id = json['cathedral_id'] as String? ?? '';
      final secret = json['cathedral_secret'] as String? ?? '';

      if (!RegExp(r'^[0-9a-f]{8}$').hasMatch(id) ||
          !RegExp(r'^[0-9a-f]{64}$').hasMatch(secret)) {
        throw VigilError('Reliquary sent back something we do not understand.');
      }

      return json;
    } on SocketException catch (e) {
      throw VigilError('Could not reach Reliquary: ${e.message}');
    } on TimeoutException {
      throw VigilError('Reliquary did not answer in time.');
    } on FormatException {
      throw VigilError('Reliquary sent back something we do not understand.');
    } finally {
      client.close(force: true);
    }
  }

  void _at(String step) {
    if (_cancelled) {
      throw VigilError('Stopped.');
    }

    if (mounted) {
      setState(() => _step = step);
    }
  }

  Future<void> _join() async {
    final typed = _typed;

    if (typed.length != _wordCount || _unknown.isNotEmpty) {
      setState(() => _problem = 'Type all $_wordCount words you were given.');
      return;
    }

    FocusManager.instance.primaryFocus?.unfocus();

    setState(() {
      _running = true;
      _problem = null;
    });

    try {
      _at('Checking the words');

      final route = await Vigil.instance.remoteWords(typed.join(' '));
      if (route == null) {
        throw VigilError(
          'These words are not right, check them against the ones you '
          'were given.',
        );
      }

      _at('Joining the flock at Reliquary');

      final cosk = <int>[];
      final hex = route['cosk'] as String;
      for (var i = 0; i < hex.length; i += 2) {
        cosk.add(int.parse(hex.substring(i, i + 2), radix: 16));
      }

      final joined = await _create(route['flock'] as String, cosk);

      _at('Setting up the tunnel to Covenant');

      await Vigil.instance.remoteStart(
        joined['cathedral_id'] as String,
        joined['cathedral_secret'] as String,
      );

      _at('Waiting for Covenant, this can take up to a minute');

      final until = DateTime.now().add(const Duration(minutes: 10));

      while (true) {
        if (_cancelled) {
          throw VigilError('Stopped.');
        }

        final state = await Vigil.instance.remoteRun();
        if (state == 3) {
          break;
        }

        if (state == -1) {
          throw VigilError('The tunnel to Covenant failed, start over.');
        }

        if (state == 2) {
          _at('Connected, receiving the configuration');
        }

        if (DateTime.now().isAfter(until)) {
          throw VigilError(
            'Covenant did not show up in time, ask for new words and start '
            'over.',
          );
        }
      }

      final config = await Vigil.instance.remoteConfig();
      await Vigil.instance.remoteStop();

      if (config == null) {
        throw VigilError('Covenant sent no configuration, start over.');
      }

      _words.clear();
      replace<void>(ReviewPage(config: config)).ignore();
    } on VigilError catch (e) {
      await Vigil.instance.remoteStop();

      if (mounted && !_cancelled) {
        setState(() {
          _running = false;
          _step = null;
          _problem = e.message;
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final count = _typed.length;
    final unknown = _unknown;

    return PushedPage(
      rubric: 'Import',
      title: 'Join with words',
      bottom: Btn(
        label: 'Join',
        expand: true,
        busy: _running,
        onTap: count == _wordCount && unknown.isEmpty ? _join : null,
      ),
      children: [
        const Text(
          'Type the words you got from the flock administrator, in the '
          'order you were given them. This device then joins the flock and '
          'gets its configuration straight from Covenant.',
          style: Styles.body,
        ),
        Pre(
          child: TextField(
            controller: _words,
            enabled: !_running,
            minLines: 6,
            maxLines: 10,
            autocorrect: false,
            enableSuggestions: false,
            enableIMEPersonalizedLearning: false,
            keyboardType: TextInputType.visiblePassword,
            style: Styles.pre,
            cursorColor: Palette.amber,
            decoration: InputDecoration.collapsed(
              hintText: 'word word word ...',
              hintStyle: Styles.pre.copyWith(color: Palette.dim),
            ),
          ),
        ),
        if (_suggestions.isNotEmpty)
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              for (final w in _suggestions)
                Btn(
                  label: w,
                  kind: BtnKind.secondary,
                  onTap: () => _complete(w),
                ),
            ],
          ),
        Text(
          unknown.isNotEmpty
              ? 'Not in the word list: ${unknown.join(', ')}'
              : '$count of $_wordCount words',
          style: Styles.detail.copyWith(
            color: unknown.isNotEmpty ? Palette.amber : null,
          ),
        ),
        if (_step != null) Notice(text: _step!),
        if (_problem != null) Notice(kind: NoticeKind.warn, text: _problem!),
        GestureDetector(
          onTap: () => setState(() => _advanced = !_advanced),
          child: Text(
            _advanced ? 'Hide the Reliquary address' : 'Use another Reliquary',
            style: Styles.detail.copyWith(color: Palette.amber),
          ),
        ),
        if (_advanced)
          Pre(
            child: TextField(
              controller: _api,
              enabled: !_running,
              autocorrect: false,
              enableSuggestions: false,
              keyboardType: TextInputType.url,
              style: Styles.pre,
              cursorColor: Palette.amber,
              decoration: const InputDecoration.collapsed(hintText: _reliquary),
            ),
          ),
      ],
    );
  }
}
