import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app.dart';
import '../vigil.dart';
import '../theme.dart';
import '../widgets/buttons.dart';
import '../widgets/marks.dart';
import '../widgets/network_map.dart';
import '../widgets/ui.dart';

bool _shown = false;

void showCall() {
  if (_shown || navigatorKey.currentState == null) {
    return;
  }

  _shown = true;
  push(const CallPage()).whenComplete(() => _shown = false);
}

Future<void> callPeer(String peer, {bool video = false}) async {
  try {
    await Vigil.instance.call(peer, video: video);
    showCall();
  } on VigilError catch (e) {
    notify(e.message);
  }
}

final _back = Object();

Future<bool> renamePeer(String peer, {bool back = false}) async {
  final context = navigatorKey.currentContext;
  if (context == null) {
    return false;
  }

  final vigil = Vigil.instance;
  final name = await sheet<Object>(
    context,
    (context) => _NamePrompt(peer, back: back),
  );

  if (name == _back) {
    return true;
  }

  if (name is! String) {
    return false;
  }

  try {
    await vigil.rename(peer, name);
  } on VigilError catch (e) {
    notify(e.message);
  }

  return false;
}

Future<bool> _safetyCode(String peer) async {
  await push<void>(_SafetyCode(peer));

  return true;
}

Widget _backTitle(BuildContext context, String title) => Transform.translate(
  offset: const Offset(-12, 0),
  child: Row(
    children: [
      GlyphBtn(
        GlyphKind.back,
        label: 'Back',
        onTap: () => Navigator.of(context).pop(_back),
      ),
      Expanded(child: sheetTitle(title)),
    ],
  ),
);

class _NamePrompt extends StatefulWidget {
  const _NamePrompt(this.peer, {this.back = false});

  final String peer;
  final bool back;

  @override
  State<_NamePrompt> createState() => _NamePromptState();
}

class _NamePromptState extends State<_NamePrompt> {
  late final _name = TextEditingController(
    text: Vigil.instance.names[widget.peer] ?? '',
  );

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final peer = widget.peer;

    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        if (widget.back)
          _backTitle(context, 'Name peer $peer')
        else
          sheetTitle('Name peer $peer'),
        const SizedBox(height: 16),
        Pre(
          child: TextField(
            controller: _name,
            autofocus: true,
            maxLength: 32,
            enableIMEPersonalizedLearning: false,
            textCapitalization: TextCapitalization.words,
            textInputAction: TextInputAction.done,
            style: Styles.pre,
            cursorColor: Palette.amber,
            decoration: InputDecoration.collapsed(
              hintText: peer,
              hintStyle: Styles.pre.copyWith(color: Palette.dim),
            ).copyWith(counterText: ''),
            onSubmitted: (value) => Navigator.of(context).pop(value),
          ),
        ),
        const SizedBox(height: 16),
        BtnPair(
          first: Btn(
            label: 'Clear',
            kind: BtnKind.secondary,
            expand: true,
            onTap: () => Navigator.of(context).pop(''),
          ),
          second: Btn(
            label: 'Save',
            expand: true,
            onTap: () => Navigator.of(context).pop(_name.text),
          ),
        ),
      ],
    );
  }
}

Future<void> peerDetails(String peer) async {
  var back = true;

  while (back) {
    final context = navigatorKey.currentContext;
    if (context == null) {
      return;
    }

    if (!context.mounted) {
      return;
    }

    final action = await sheet<String>(
      context,
      (context) => _PeerDetails(peer),
    );

    back = false;

    if (action == 'name') {
      back = await renamePeer(peer, back: true);
    }

    if (action == 'code') {
      back = await _safetyCode(peer);
    }
  }
}

String _safetyState(String peer) {
  final vigil = Vigil.instance;

  if (vigil.safetyActive(peer)) {
    return 'On';
  }

  if (vigil.safetyOn(peer)) {
    return 'On, not set yet';
  }

  return 'Off';
}

class _PeerDetails extends StatelessWidget {
  const _PeerDetails(this.peer);

  final String peer;

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;
    final active = vigil.safetyActive(peer);

    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        sheetTitle(vigil.name(peer)),
        if (vigil.name(peer) != peer) ...[
          const SizedBox(height: 6),
          Text(peer, style: Styles.detail),
        ],
        const SizedBox(height: 16),
        LinkList(
          rows: [
            LinkRow(
              'Name',
              () => Navigator.of(context).pop('name'),
              detail: vigil.names[peer] ?? 'Not named',
              glyph: GlyphKind.person,
            ),
            LinkRow(
              'Safety code',
              () => Navigator.of(context).pop('code'),
              detail: _safetyState(peer),
              glyph: GlyphKind.shield,
              tint: active ? Palette.good : null,
            ),
          ],
        ),
      ],
    );
  }
}

const _safetyWords = 5;

String _setAgo(int since) {
  final days = DateTime.now()
      .difference(DateTime.fromMillisecondsSinceEpoch(since))
      .inDays;

  if (days < 1) {
    return 'This safety code was set today.';
  }

  if (days == 1) {
    return 'This safety code was set 1 day ago.';
  }

  return 'This safety code was set $days days ago.';
}

const _codeStyle = TextStyle(
  fontFamily: Fonts.mono,
  fontSize: 15,
  height: 1.4,
  color: Palette.fg,
);

class _SafetyCode extends StatefulWidget {
  const _SafetyCode(this.peer);

  final String peer;

  @override
  State<_SafetyCode> createState() => _SafetyCodeState();
}

class _SafetyCodeState extends State<_SafetyCode> {
  final _words = TextEditingController();
  StreamSubscription<Status>? _status;
  SafetyCode? _state;
  Set<String> _list = const {};
  List<String> _sorted = const [];
  String _mine = '';
  String _exchange = '';
  bool _redo = false;
  bool _started = false;

  @override
  void initState() {
    super.initState();
    _words.addListener(() => setState(() {}));
    _status = Vigil.instance.statuses.listen(_changed);
    _load();

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
    _status?.cancel();
    _words.clear();
    _words.dispose();
    Vigil.instance.safetyStop().catchError((Object _) {});
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
    if (text.isEmpty || text.endsWith(' ')) {
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

  void _changed(Status status) {
    final chat = status.chat;

    if (!_started || chat == null || chat.safetyPeer != widget.peer) {
      return;
    }

    if (chat.safetyState == _exchange) {
      return;
    }

    setState(() => _exchange = chat.safetyState);

    if (_exchange == 'done') {
      _started = false;
      _redo = false;
      _words.clear();
      _settle();
    }
  }

  Future<void> _settle() async {
    for (var idx = 0; idx < 10; idx++) {
      await _load();

      if (!mounted) {
        return;
      }

      if (_state?.set ?? false) {
        notify('Safety code set', warn: false);
        return;
      }

      await Future<void>.delayed(const Duration(milliseconds: 500));
    }
  }

  Future<void> _load({bool clear = false, bool? on}) async {
    try {
      final state = await Vigil.instance.safetyCode(
        widget.peer,
        clear: clear,
        on: on,
      );

      if (!mounted) {
        return;
      }

      setState(() => _state = state);

      if (state.on && (!state.set || _redo) && _mine.isEmpty) {
        await _begin();
      }

      if (!state.on) {
        await _stop();
      }
    } on VigilError catch (e) {
      notify(e.message);
    }
  }

  Future<void> _begin() async {
    try {
      final mine = await Vigil.instance.safetyBegin(widget.peer);

      if (mounted) {
        setState(() {
          _mine = mine;
          _exchange = '';
          _started = false;
        });
      }
    } on VigilError catch (e) {
      notify(e.message);
    }
  }

  Future<void> _stop() async {
    await Vigil.instance.safetyStop();

    if (mounted) {
      setState(() {
        _mine = '';
        _exchange = '';
        _started = false;
        _redo = false;
      });
    }
  }

  Future<void> _connect() async {
    FocusManager.instance.primaryFocus?.unfocus();

    try {
      await Vigil.instance.safetyWords(widget.peer, _typed.join(' '));

      if (mounted) {
        setState(() {
          _started = true;
          _exchange = 'connecting';
        });
      }
    } on VigilError catch (e) {
      notify(e.message);
    }
  }

  Future<void> _again() async {
    setState(() {
      _redo = true;
      _mine = '';
    });

    _words.clear();
    await _begin();
  }

  Future<void> _copy() async {
    await Clipboard.setData(ClipboardData(text: _mine));
    notify('Copied', warn: false);
  }

  Future<void> _leave() async {
    final name = Vigil.instance.name(widget.peer);
    final leave = await confirm(
      title: 'Stop setting the safety code?',
      text:
          'Leaving this page stops setting the safety code with $name. Both '
          'of you then have to start over with new words.',
      action: 'Stop',
      risky: true,
    );

    if (leave && mounted) {
      Navigator.of(context).pop();
    }
  }

  Widget _exchangeNotice(String name) {
    if (_exchange == 'connecting') {
      return Notice(
        text:
            'Waiting for $name. This stops after two minutes.',
      );
    }

    return const Notice(
      kind: NoticeKind.warn,
      text:
          'No safety code was set. Check the words and try again.',
    );
  }

  List<Widget> _exchangeView(bool busy) {
    final count = _typed.length;
    final unknown = _unknown;

    return [
      Panel(
        padding: const EdgeInsets.fromLTRB(16, 12, 4, 12),
        child: Row(
          children: [
            Expanded(child: Text(_mine, style: _codeStyle)),
            GlyphBtn(
              GlyphKind.copy,
              label: 'Copy your words',
              onTap: _mine.isEmpty ? null : _copy,
            ),
          ],
        ),
      ),
      Pre(
        child: TextField(
          controller: _words,
          enabled: !busy,
          minLines: 2,
          maxLines: 3,
          autocorrect: false,
          enableSuggestions: false,
          enableIMEPersonalizedLearning: false,
          keyboardType: TextInputType.visiblePassword,
          style: _codeStyle,
          cursorColor: Palette.amber,
          decoration: InputDecoration.collapsed(
            hintText: 'Their words',
            hintStyle: _codeStyle.copyWith(color: Palette.dim),
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
            : '$count of $_safetyWords words',
        style: Styles.detail.copyWith(
          color: unknown.isNotEmpty ? Palette.amber : null,
        ),
      ),
    ];
  }

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;
    final state = _state;
    final on = state?.on ?? vigil.safetyOn(widget.peer);
    final set = state?.set ?? false;
    final name = vigil.name(widget.peer);
    final ready = _typed.length == _safetyWords && _unknown.isEmpty;
    final busy = _started && _exchange == 'connecting';
    final exchanging = on && (!set || _redo);
    final shown = on && set && !_redo;
    Widget? bottom;

    if (exchanging) {
      bottom = Btn(
        label: 'Start',
        expand: true,
        busy: busy,
        onTap: ready && _mine.isNotEmpty ? _connect : null,
      );
    }

    if (shown) {
      bottom = Btn(label: 'Renew', expand: true, onTap: _again);
    }

    return PopScope(
      canPop: !busy,
      onPopInvokedWithResult: (popped, _) {
        if (!popped) {
          _leave();
        }
      },
      child: PushedPage(
        rubric: name,
        title: 'Safety code',
        bottom: bottom,
        children: [
          if (!shown)
            Text(
              'Sanctum tunnels are always PQ-secure and end-to-end encrypted. '
              'By using a safety code you add one additional layer of '
              'security to your key exchange, as protection against your '
              'organizer accidentally leaking your unique device '
              'Key-Encryption-Key (KEK).'
              '\n\n'
              'When enabling this feature, a strong 256-bit secret only known '
              'to you and $name is mixed in together with all other keys.',
              style: Styles.body,
            ),
          Row(
            children: [
              Expanded(
                child: Text('Use a safety code', style: Styles.cardTitle),
              ),
              Toggle(
                on: on,
                label: 'Use a safety code',
                onTap: busy ? null : () => _load(on: !on),
              ),
            ],
          ),
          if (shown) ...[
            Panel(
              padding: const EdgeInsets.fromLTRB(16, 14, 16, 14),
              child: Text(state?.code ?? '', style: _codeStyle),
            ),
            Text(_setAgo(state?.since ?? 0), style: Styles.detail),
            Text(
              'This code only identifies the safety code, it is not the '
              'secret itself. Compare the code with $name.',
              style: Styles.detail,
            ),
          ],
          if (exchanging) ..._exchangeView(busy),
          if (exchanging &&
              (_exchange == 'connecting' || _exchange == 'failed'))
            _exchangeNotice(name),
        ],
      ),
    );
  }
}

Future<T?> sheet<T>(BuildContext context, WidgetBuilder builder) =>
    showModalBottomSheet<T>(
      context: context,
      isScrollControlled: true,
      useSafeArea: true,
      backgroundColor: const Color(0x00000000),
      barrierColor: Palette.veil,
      elevation: 0,
      builder: (context) => Padding(
        padding: EdgeInsets.only(
          bottom: MediaQuery.viewInsetsOf(context).bottom,
        ),
        child: DecoratedBox(
          decoration: sheetDecoration,
          child: Padding(
            padding: EdgeInsets.fromLTRB(
              Metrics.edge,
              14,
              Metrics.edge,
              MediaQuery.paddingOf(context).bottom + 18,
            ),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                const SheetHandle(),
                const SizedBox(height: 20),
                // Bounded by the screen minus the keyboard, so a sheet can
                // let a Flexible part give way when the keyboard comes up.
                Flexible(child: builder(context)),
              ],
            ),
          ),
        ),
      ),
    );

Widget sheetTitle(String title) => Text(title, style: Styles.barTitle);

class CallPage extends StatefulWidget {
  const CallPage({super.key});

  @override
  State<CallPage> createState() => _CallPageState();
}

class _CallPageState extends State<CallPage> {
  PhoneCall? _last;
  bool _closing = false;

  @override
  void initState() {
    super.initState();

    Future.delayed(const Duration(seconds: 5), () {
      if (mounted && _last == null) {
        _close();
      }
    });
  }

  Future<void> _run(Future<void> Function() action) async {
    try {
      await action();
    } on VigilError catch (e) {
      notify(e.message);
    }
  }

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;

    return StreamBuilder<Status>(
      stream: vigil.statuses,
      initialData: vigil.status,
      builder: (context, snapshot) {
        final status = snapshot.data!;
        final phone = status.phone;

        if (phone != null) {
          _last = phone;
        } else if (_last != null) {
          _close();
        }

        final call = phone ?? _last;
        final peer = call?.peer ?? '';
        final ringing = phone?.incoming == true && phone?.state == 'ringing';
        final offered = status.voice?.remoteCamera ?? false;

        if (videoCalls &&
            phone != null &&
            !ringing &&
            (phone.video || offered)) {
          return PopScope(
            canPop: false,
            child: _VideoCall(
              call: phone,
              voice: status.voice,
              state: _state(phone),
              keys: _videoKeys(phone),
            ),
          );
        }

        return PopScope(
          canPop: phone == null,
          child: Ground(
            child: SafeArea(
              child: Padding(
                padding: const EdgeInsets.fromLTRB(
                  Metrics.edge,
                  18,
                  Metrics.edge,
                  28,
                ),
                child: Column(
                  children: [
                    Text(
                      ringing
                          ? (offered ? 'INCOMING VIDEO CALL' : 'INCOMING CALL')
                          : 'CALL',
                      style: Styles.rubric,
                    ),
                    const SizedBox(height: 12),
                    Expanded(
                      child: _PeerMark(
                        peer: peer,
                        name: vigil.name(peer),
                        state: _liveness(phone),
                        label: Row(
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            StateDot(_liveness(phone)),
                            const SizedBox(width: 10),
                            Text(
                              _state(phone),
                              style: Styles.rubric.copyWith(
                                color: phone?.state == 'active'
                                    ? Palette.good
                                    : Palette.dim,
                              ),
                            ),
                          ],
                        ),
                      ),
                    ),
                    const SizedBox(height: 12),
                    if (phone != null && status.voice?.running == true)
                      Panel(
                        padding: const EdgeInsets.fromLTRB(14, 14, 14, 12),
                        child: CallMap(peer: peer, voice: status.voice!),
                      ),
                    const SizedBox(height: 28),
                    if (phone != null) _keys(phone, offered),
                  ],
                ),
              ),
            ),
          ),
        );
      },
    );
  }

  Widget _keys(PhoneCall call, bool offered) {
    final vigil = Vigil.instance;

    if (call.incoming && call.state == 'ringing') {
      return Row(
        mainAxisAlignment: MainAxisAlignment.spaceEvenly,
        children: [
          _CallKey(
            glyph: GlyphKind.hangup,
            label: 'Decline',
            onTap: () => _run(vigil.decline),
          ),
          if (offered && videoCalls)
            _CallKey(
              glyph: GlyphKind.call,
              label: 'Voice',
              onTap: () => _run(vigil.answer),
            ),
          if (offered && videoCalls)
            _CallKey(
              glyph: GlyphKind.video,
              label: 'Video',
              primary: true,
              onTap: () => _run(() => vigil.answer(video: true)),
            )
          else
            _CallKey(
              glyph: GlyphKind.call,
              label: 'Answer',
              primary: true,
              onTap: () => _run(vigil.answer),
            ),
        ],
      );
    }

    return Row(
      mainAxisAlignment: MainAxisAlignment.spaceEvenly,
      children: [
        _CallKey(
          glyph: GlyphKind.mute,
          label: 'Mute',
          on: call.muted,
          onTap: () => _run(() => vigil.mute(!call.muted)),
        ),
        _CallKey(
          glyph: GlyphKind.speaker,
          label: 'Speaker',
          on: call.speaker,
          onTap: () => _run(() => vigil.speaker(!call.speaker)),
        ),
        _CallKey(
          glyph: GlyphKind.hangup,
          label: 'Hang up',
          primary: true,
          onTap: () => _run(vigil.hangup),
        ),
      ],
    );
  }

  Widget _videoKeys(PhoneCall call) {
    final vigil = Vigil.instance;

    return Row(
      mainAxisAlignment: MainAxisAlignment.spaceEvenly,
      children: [
        _CallKey(
          glyph: GlyphKind.mute,
          label: 'Mute',
          compact: true,
          on: call.muted,
          onTap: () => _run(() => vigil.mute(!call.muted)),
        ),
        _CallKey(
          glyph: GlyphKind.video,
          label: 'Camera',
          compact: true,
          on: call.video,
          onTap: () => _run(() => vigil.camera(!call.video)),
        ),
        if (call.video)
          _CallKey(
            glyph: GlyphKind.flip,
            label: 'Flip',
            compact: true,
            onTap: () => _run(vigil.flip),
          ),
        _CallKey(
          glyph: GlyphKind.speaker,
          label: 'Speaker',
          compact: true,
          on: call.speaker,
          onTap: () => _run(() => vigil.speaker(!call.speaker)),
        ),
        _CallKey(
          glyph: GlyphKind.hangup,
          label: 'Hang up',
          compact: true,
          primary: true,
          onTap: () => _run(vigil.hangup),
        ),
      ],
    );
  }

  Liveness _liveness(PhoneCall? phone) => switch (phone?.state) {
    null => Liveness.off,
    'active' => Liveness.online,
    _ => Liveness.pending,
  };

  String _state(PhoneCall? phone) {
    switch (phone?.state) {
      case null:
        if (_last == null) {
          return 'CALLING';
        }

        return 'CALL ENDED';
      case 'ringing':
      case 'alerting':
        return 'RINGING';
      case 'dialing':
        return 'CALLING';
      case 'connecting':
        return 'CONNECTING';
      default:
        final since = DateTime.fromMillisecondsSinceEpoch(phone!.since);
        return duration(DateTime.now().difference(since).inSeconds);
    }
  }

  void _close() {
    if (_closing) {
      return;
    }

    _closing = true;

    Future.delayed(const Duration(seconds: 1), () {
      if (mounted) {
        Navigator.of(context).pop();
      }
    });
  }
}

class _PeerMark extends StatelessWidget {
  const _PeerMark({
    required this.peer,
    required this.name,
    required this.state,
    required this.label,
  });

  final String peer;
  final String name;
  final Liveness state;
  final Widget label;

  @override
  Widget build(BuildContext context) {
    final live = state != Liveness.off;

    return Stack(
      children: [
        if (live)
          Positioned.fill(
            child: NetBackdrop(
              intensity: state == Liveness.online ? 0.6 : 1,
              centered: true,
            ),
          ),
        Center(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTap: peer.isEmpty ? null : () => renamePeer(peer),
                child: Column(
                  children: [
                    Avatar(
                      peer,
                      size: 112,
                      margin: const EdgeInsets.only(bottom: 14),
                    ),
                    Padding(
                      padding: const EdgeInsets.symmetric(
                        horizontal: Metrics.edge,
                      ),
                      child: PeerName(
                        peer,
                        name: name,
                        style: Styles.title.copyWith(
                          color: live ? Palette.amber : Palette.dim,
                        ),
                        shrink: 16,
                      ),
                    ),
                    if (name != peer) ...[
                      const SizedBox(height: 6),
                      Text(peer, style: Styles.detail),
                    ],
                  ],
                ),
              ),
              const SizedBox(height: 12),
              label,
            ],
          ),
        ),
      ],
    );
  }
}

class _CallKey extends StatelessWidget {
  const _CallKey({
    required this.glyph,
    required this.label,
    required this.onTap,
    this.primary = false,
    this.on = false,
    this.compact = false,
  });

  final GlyphKind glyph;
  final String label;
  final VoidCallback onTap;
  final bool primary;
  final bool on;
  final bool compact;

  @override
  Widget build(BuildContext context) {
    return Pressable(
      onTap: onTap,
      label: label,
      builder: (context, pressed) {
        final lit = primary || on;
        final fill = primary
            ? (pressed ? Palette.amber2 : Palette.amber)
            : on
            ? Palette.amberWash
            : (pressed ? Palette.bg3 : Palette.bg2);

        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            SizedBox(
              width: compact ? 56 : 72,
              height: compact ? 56 : 72,
              child: CustomPaint(
                painter: RoundPainter(
                  fill: fill,
                  stroke: lit ? Palette.amber : Palette.line2,
                  radius: compact ? 28 : 36,
                ),
                child: Center(
                  child: Glyph(
                    glyph,
                    size: compact ? 22 : 26,
                    color: primary
                        ? Palette.bg
                        : on
                        ? Palette.amber
                        : Palette.fg2,
                  ),
                ),
              ),
            ),
            const SizedBox(height: 10),
            Text(
              label,
              style: Styles.tab.copyWith(color: lit ? Palette.fg : Palette.dim),
            ),
          ],
        );
      },
    );
  }
}

class _VideoCall extends StatefulWidget {
  const _VideoCall({
    required this.call,
    required this.voice,
    required this.state,
    required this.keys,
  });

  final PhoneCall call;
  final VoiceStatus? voice;
  final String state;
  final Widget keys;

  @override
  State<_VideoCall> createState() => _VideoCallState();
}

class _VideoCallState extends State<_VideoCall> {
  (int, int)? _textures;
  VideoState? _video;
  Timer? _poll;

  @override
  void initState() {
    super.initState();

    unawaited(_open());
    _poll = Timer.periodic(
      const Duration(milliseconds: 500),
      (_) => _refresh(),
    );
  }

  Future<void> _open() async {
    try {
      final textures = await Vigil.instance.videoTextures();

      if (mounted) {
        setState(() => _textures = textures);
      }
    } on VigilError catch (e) {
      notify(e.message);
    }
  }

  Future<void> _refresh() async {
    try {
      final video = await Vigil.instance.videoState();

      if (mounted) {
        setState(() => _video = video);
      }
    } on VigilError catch (_) {
      return;
    }
  }

  @override
  void dispose() {
    _poll?.cancel();
    unawaited(Vigil.instance.videoRelease().catchError((Object _) {}));
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final textures = _textures;
    final video = _video;
    final remote = widget.voice?.remoteCamera ?? false;
    final width = MediaQuery.sizeOf(context).width;

    final showRemote =
        textures != null &&
        remote &&
        video != null &&
        video.width > 0 &&
        video.height > 0;

    return ColoredBox(
      color: Colors.black,
      child: Stack(
        fit: StackFit.expand,
        children: [
          if (showRemote)
            Center(
              child: RotatedBox(
                quarterTurns: video.rotation,
                child: AspectRatio(
                  aspectRatio: video.width / video.height,
                  child: Texture(textureId: textures.$1),
                ),
              ),
            )
          else
            Center(
              child: Text(
                remote ? 'WAITING FOR VIDEO' : 'CAMERA OFF',
                style: Styles.rubric,
              ),
            ),
          if (textures != null && widget.call.video && video != null)
            Positioned(
              top: MediaQuery.paddingOf(context).top + 16,
              right: Metrics.edge,
              width: width * 0.3,
              child: AspectRatio(
                aspectRatio: 9 / 16,
                child: ClipRect(
                  child: Transform.flip(
                    flipX: video.localMirror,
                    child: RotatedBox(
                      quarterTurns: video.localRotation,
                      child: FittedBox(
                        fit: BoxFit.cover,
                        child: SizedBox(
                          width: video.localWidth.toDouble(),
                          height: video.localHeight.toDouble(),
                          child: Texture(textureId: textures.$2),
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ),
          Positioned(
            top: MediaQuery.paddingOf(context).top + 16,
            left: Metrics.edge,
            right: width * 0.3 + Metrics.edge * 2,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                PeerName(
                  widget.call.peer,
                  style: Styles.cardTitle.copyWith(color: Colors.white),
                  shrink: 12,
                ),
                const SizedBox(height: 4),
                Text(widget.state, style: Styles.rubric),
              ],
            ),
          ),
          Positioned(
            left: Metrics.edge,
            right: Metrics.edge,
            bottom: MediaQuery.paddingOf(context).bottom + 24,
            child: widget.keys,
          ),
        ],
      ),
    );
  }
}
