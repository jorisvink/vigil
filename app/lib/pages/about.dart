import 'package:flutter/widgets.dart';

import '../app.dart';
import '../vigil.dart';
import '../theme.dart';
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
        const GroupLabel('The Community', index: 1),
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
