import 'dart:io';

import 'package:flutter/widgets.dart';

import '../app.dart';
import '../vigil.dart';
import '../theme.dart';
import '../widgets/buttons.dart';
import '../widgets/marks.dart';
import '../widgets/ui.dart';

const _example = '''vigil 1
instance office
tunnel 10.77.0.2/24 1408
peer 198.51.100.1:4500
secret <64 hex characters>
route 0.0.0.0/0
dns 10.77.0.1''';

class AboutPage extends StatelessWidget {
  const AboutPage({super.key});

  @override
  Widget build(BuildContext context) {
    return TabPage(
      index: 5,
      rubric: 'About',
      title: 'Vigil',
      children: [
        Panel(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text(
                'Vigil - The Sanctum companion app',
                style: TextStyle(
                  fontFamily: Fonts.head,
                  fontSize: 19,
                  height: 1.35,
                  color: Palette.fg,
                ),
              ),
              Text('Copyright (c) 2026 Joris Vink', style: Styles.rubric),
              const SizedBox(height: 16),
              FutureBuilder<List<String>>(
                future: Future.wait([
                  Vigil.instance.version(),
                  Vigil.instance.kyrkaVersion(),
                ]),
                builder: (context, versions) => Text(
                  'Vigil: ${versions.data?[0] ?? '-'}\n'
                  'libkyrka: ${versions.data?[1] ?? '-'}',
                  style: Styles.rubric,
                ),
              ),
            ],
          ),
        ),
        const GroupLabel('On this device', index: 1),
        if (Platform.isIOS) const Panel(child: _Candle()),
        const Panel(child: _Erase()),
        const GroupLabel('The Community', index: 2),
        LinkList(
          rows: [
            LinkRow(
              'sanctorum.se',
              () => open('https://sanctorum.se'),
              detail: 'The sanctum project',
            ),
            LinkRow(
              'The Reliquary',
              () => open('https://reliquary.se'),
              detail: 'Community cathedral network',
            ),
          ],
        ),
        const SizedBox(height: 10),
        Center(
          child: Column(
            children: [
              const SolidCross(),
              const SizedBox(height: 12),
              Text.rich(
                const TextSpan(children: [TextSpan(text: 'ITE, MISSA EST')]),
                style: Styles.rubric.copyWith(fontSize: 10),
              ),
            ],
          ),
        ),
      ],
    );
  }
}

class _Erase extends StatefulWidget {
  const _Erase();

  @override
  State<_Erase> createState() => _EraseState();
}

class _EraseState extends State<_Erase> {
  bool _busy = false;

  Future<void> _erase() async {
    if (!await confirm(
      title: 'Erase all data',
      text:
          'This stops every tunnel and Gospel and wipes all configurations, '
          'chats, files, keys, names and settings from this device. It '
          'cannot be undone. Vigil then '
          '${Platform.isIOS ? 'closes' : 'starts over'} as if it was just '
          'installed.',
      action: 'Erase',
      risky: true,
    )) {
      return;
    }

    setState(() => _busy = true);

    try {
      await Vigil.instance.eraseAll();
    } on VigilError catch (e) {
      notify(e.message);

      if (mounted) {
        setState(() => _busy = false);
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text('Erase all data', style: Styles.cardTitle),
        const SizedBox(height: 3),
        const Text(
          'Removes everything Vigil keeps on this device. ',
          style: Styles.detail,
        ),
        const SizedBox(height: 14),
        Btn(
          label: 'Erase all data',
          kind: BtnKind.secondary,
          expand: true,
          onTap: _busy ? null : _erase,
        ),
      ],
    );
  }
}

class _Candle extends StatefulWidget {
  const _Candle();

  @override
  State<_Candle> createState() => _CandleState();
}

class _CandleState extends State<_Candle> {
  static const _key = 'candle';

  bool _on = false;

  @override
  void initState() {
    super.initState();

    Vigil.instance.flag(_key).then((on) {
      if (mounted) {
        setState(() => _on = on);
      }
    }, onError: (Object _) {});
  }

  Future<void> _toggle() async {
    final on = !_on;

    setState(() => _on = on);
    await Vigil.instance.flag(_key, on).catchError((Object _) => on);
  }

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text('Lock Screen candle', style: Styles.cardTitle),
              const SizedBox(height: 3),
              const Text(
                'A Live Activity on the Lock Screen while a tunnel or '
                'Gospel runs.',
                style: Styles.detail,
              ),
            ],
          ),
        ),
        Toggle(on: _on, label: 'Lock Screen candle', onTap: _toggle),
      ],
    );
  }
}
