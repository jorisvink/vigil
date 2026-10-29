import 'dart:math';

import 'package:flutter/widgets.dart';

import '../app.dart';
import '../theme.dart';
import '../vigil.dart';
import 'buttons.dart';
import 'marks.dart';
import 'ui.dart';

class LinkNotice extends StatelessWidget {
  const LinkNotice({super.key});

  @override
  Widget build(BuildContext context) {
    return const Notice(
      kind: NoticeKind.warn,
      text:
          'Vigil lost contact with its tunnel, calls and messages may not '
          'go out. Reconnect the tunnel if this does not clear up.',
    );
  }
}

class LinkOverlay extends StatefulWidget {
  const LinkOverlay({super.key});

  @override
  State<LinkOverlay> createState() => _LinkOverlayState();
}

class _LinkOverlayState extends State<LinkOverlay> {
  static const _lines = [
    'VIGILATE ET ORATE',
    'CUSTOS, QUID DE NOCTE?',
    'EXSPECTANS EXSPECTAVI DOMINUM',
    'EXSPECTA DOMINUM',
    'MANE NOBISCUM, DOMINE',
    'IN TE, DOMINE, SPERAVI',
    'FIAT VOLUNTAS TUA',
    'SURSUM CORDA',
    'DOMINUS VOBISCUM',
    'PAX TECUM',
    'ORA PRO NOBIS',
    'VENI, SANCTE SPIRITUS',
  ];

  static final _random = Random();

  bool _shown = false;
  String _line = _lines.first;

  @override
  Widget build(BuildContext context) {
    return StreamBuilder<Status>(
      stream: Vigil.instance.statuses,
      initialData: Vigil.instance.status,
      builder: (context, snapshot) {
        final status = snapshot.data!;
        final shown =
            status.running && status.linking && status.connecting == null;

        if (shown && !_shown) {
          _line = _lines[_random.nextInt(_lines.length)];
        }

        _shown = shown;

        return IgnorePointer(
          ignoring: !shown,
          child: AnimatedOpacity(
            opacity: shown ? 1 : 0,
            duration: const Duration(milliseconds: 150),
            child: ColoredBox(
              color: const Color(0xcc0b0c0e),
              child: Center(
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: Metrics.edge),
                  child: Panel(
                    child: Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        const SolidCross(glow: 1),
                        const SizedBox(width: 12),
                        Flexible(
                          child: Text(
                            _line,
                            style: Styles.rubric,
                            textAlign: TextAlign.center,
                          ),
                        ),
                        const SizedBox(width: 12),
                        const SolidCross(glow: 1),
                      ],
                    ),
                  ),
                ),
              ),
            ),
          ),
        );
      },
    );
  }
}

class BackgroundNotice extends StatelessWidget {
  const BackgroundNotice({super.key});

  Future<void> _allow() async {
    try {
      await Vigil.instance.allowBackground();
    } on VigilError catch (e) {
      notify(e.message);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        const Notice(
          kind: NoticeKind.warn,
          text:
              'Android may stop Vigil while it runs in the background, '
              'tunnels then drop and calls and messages no longer reach '
              'you.',
        ),
        const SizedBox(height: 8),
        Btn(
          label: 'Allow in background',
          kind: BtnKind.secondary,
          expand: true,
          onTap: _allow,
        ),
      ],
    );
  }
}
