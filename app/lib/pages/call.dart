import 'dart:async';

import 'package:flutter/material.dart';

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

Future<void> renamePeer(String peer) async {
  final context = navigatorKey.currentContext;
  if (context == null) {
    return;
  }

  final vigil = Vigil.instance;
  final name = await sheet<String>(context, (context) => _NamePrompt(peer));

  if (name == null) {
    return;
  }

  try {
    await vigil.rename(peer, name);
  } on VigilError catch (e) {
    notify(e.message);
  }
}

class _NamePrompt extends StatefulWidget {
  const _NamePrompt(this.peer);

  final String peer;

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
        sheetTitle(
          isGroup(peer) ? 'Name group ${peer.substring(1)}' : 'Name peer $peer',
        ),
        const SizedBox(height: 16),
        Pre(
          child: TextField(
            controller: _name,
            autofocus: true,
            maxLength: 32,
            textCapitalization: TextCapitalization.words,
            textInputAction: TextInputAction.done,
            style: Styles.pre,
            cursorColor: Palette.amber,
            decoration: InputDecoration.collapsed(
              hintText: isGroup(peer)
                  ? 'Group ${peer.substring(1)}'
                  : 'Peer $peer',
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

Future<T?> sheet<T>(BuildContext context, WidgetBuilder builder) =>
    showModalBottomSheet<T>(
      context: context,
      isScrollControlled: true,
      backgroundColor: const Color(0x00000000),
      barrierColor: const Color(0xcc0b0c0e),
      elevation: 0,
      builder: (context) => Padding(
        padding: EdgeInsets.only(
          bottom: MediaQuery.viewInsetsOf(context).bottom,
        ),
        child: CustomPaint(
          painter: const CutPainter(fill: Palette.bg2, stroke: Palette.line2),
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
                Center(
                  child: Container(width: 32, height: 3, color: Palette.amber),
                ),
                const SizedBox(height: 20),
                builder(context),
              ],
            ),
          ),
        ),
      ),
    );

Widget sheetTitle(String title) => Row(
  children: [
    const RubricCross(fill: Palette.bg2),
    const SizedBox(width: 12),
    Expanded(child: Text(title, style: Styles.cardTitle)),
  ],
);

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
          child: DotGrid(
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
                        name: vigil.names[peer],
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
  final String? name;
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
                    Text(
                      peer.toUpperCase(),
                      style: Styles.title.copyWith(
                        fontFamily: Fonts.mono,
                        fontSize: 38,
                        color: live ? Palette.amber : Palette.dim,
                      ),
                    ),
                    if (name != null) ...[
                      const SizedBox(height: 6),
                      Text(
                        name!,
                        style: Styles.title,
                        overflow: TextOverflow.ellipsis,
                      ),
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
                painter: CutPainter(
                  fill: fill,
                  stroke: lit ? Palette.amber : Palette.line2,
                  cut: compact ? 11 : 14,
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
              label.toUpperCase(),
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
    final vigil = Vigil.instance;
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
                Text(
                  vigil.name(widget.call.peer),
                  style: peerStyle(
                    Styles.cardTitle.copyWith(color: Colors.white),
                    vigil.picked(widget.call.peer),
                  ),
                  overflow: TextOverflow.ellipsis,
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
