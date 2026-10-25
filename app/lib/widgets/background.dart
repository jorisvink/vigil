import 'package:flutter/widgets.dart';

import '../app.dart';
import '../vigil.dart';
import 'buttons.dart';
import 'ui.dart';

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
