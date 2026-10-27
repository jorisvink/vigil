import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app.dart';
import '../vigil.dart';
import '../theme.dart';
import '../widgets/buttons.dart';
import '../widgets/marks.dart';
import '../widgets/ui.dart';

class LogPage extends StatelessWidget {
  const LogPage({super.key});

  void _copy(List<LogLine> log) {
    Clipboard.setData(
      ClipboardData(text: log.map((l) => '${l.when} ${l.line}').join('\n')),
    );
    notify('Log copied.', warn: false);
  }

  @override
  Widget build(BuildContext context) {
    return StreamBuilder<Status>(
      stream: Vigil.instance.statuses,
      initialData: Vigil.instance.status,
      builder: (context, snapshot) {
        final log = snapshot.data!.log;

        return TabPage(
          index: 4,
          rubric: 'Log',
          title: 'Log',
          glow: snapshot.data!.online,
          actions: [
            if (log.isNotEmpty)
              GlyphBtn(
                GlyphKind.copy,
                label: 'Copy the log',
                onTap: () => _copy(log),
              ),
          ],
          children: [
            if (log.isEmpty)
              const EmptyState(
                glyph: GlyphKind.log,
                title: 'Nothing logged yet',
                text:
                    'What the tunnel is up to shows up here once one '
                    'is connected.',
              )
            else
              Pre(
                child: SelectableText.rich(
                  TextSpan(
                    children: [
                      const TextSpan(
                        text: r'$ ',
                        style: TextStyle(color: Palette.amber),
                      ),
                      const TextSpan(text: 'tail -f /vigil/log\n'),
                      for (final l in log) ...[
                        TextSpan(
                          text: '${l.when}  ',
                          style: const TextStyle(color: Palette.amberDim),
                        ),
                        TextSpan(text: '${l.line}\n'),
                      ],
                      const TextSpan(
                        text: '█',
                        style: TextStyle(color: Palette.amber),
                      ),
                    ],
                  ),
                  style: Styles.pre.copyWith(fontSize: 11.5),
                ),
              ),
          ],
        );
      },
    );
  }
}
