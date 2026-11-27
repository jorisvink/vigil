import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app.dart';
import '../theme.dart';
import '../vigil.dart';
import '../widgets/buttons.dart';
import '../widgets/marks.dart';

Future<void> toggleVoice(String chat) async {
  final vigil = Vigil.instance;

  try {
    if (vigil.status.groupVoice.on(chat)) {
      await vigil.groupVoiceOff();
    } else {
      await vigil.groupVoiceOn(chat);
    }
  } on VigilError catch (e) {
    notify(e.message);
  }
}

Future<void> _mute(bool on) async {
  try {
    await Vigil.instance.groupVoiceMute(on);
  } on VigilError catch (e) {
    notify(e.message);
  }
}

Future<void> _earpiece(bool on) async {
  try {
    await Vigil.instance.groupVoiceEarpiece(on);
  } on VigilError catch (e) {
    notify(e.message);
  }
}

String voiceLine(GroupVoice voice, {bool? talking}) {
  final vigil = Vigil.instance;

  if (!voice.joined) {
    return 'Turning on voice…';
  }

  if (voice.ducked) {
    return 'Voice is on hold during your call';
  }

  if (talking ?? voice.talking) {
    return 'You are talking';
  }

  final speakers = voice.speakers;

  if (speakers.isNotEmpty) {
    final name = vigil.name(speakers.first.peer);

    if (speakers.length == 1) {
      return '$name is talking';
    }

    return '$name +${speakers.length - 1} talking';
  }

  final here = voice.members.where((m) => m.online).length;
  final line = here == 0
      ? 'Nobody else has voice on'
      : here == 1
      ? '1 other has voice on'
      : '$here others have voice on';

  if (voice.muted) {
    return 'Muted, $line';
  }

  return line;
}

class VoiceMark extends StatefulWidget {
  const VoiceMark({super.key, required this.voice, this.size = 14});

  final GroupVoice voice;
  final double size;

  @override
  State<VoiceMark> createState() => _VoiceMarkState();
}

class _VoiceMarkState extends State<VoiceMark>
    with SingleTickerProviderStateMixin {
  late final _beat = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 600),
  );

  @override
  void initState() {
    super.initState();
    _animate();
  }

  @override
  void didUpdateWidget(VoiceMark old) {
    super.didUpdateWidget(old);
    _animate();
  }

  void _animate() {
    if (widget.voice.busy && !_beat.isAnimating) {
      _beat.repeat(reverse: true);
    }

    if (!widget.voice.busy && _beat.isAnimating) {
      _beat.stop();
      _beat.value = 1;
    }
  }

  @override
  void dispose() {
    _beat.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    var tint = Palette.good;

    if (widget.voice.muted) {
      tint = Palette.dim;
    }

    if (widget.voice.busy) {
      tint = Palette.amber;
    }

    return Semantics(
      label: widget.voice.busy ? 'Someone is talking' : 'Voice is on',
      child: FadeTransition(
        opacity: Tween(begin: 0.35, end: 1.0).animate(_beat),
        child: Glyph(
          widget.voice.muted ? GlyphKind.channelOff : GlyphKind.channel,
          size: widget.size,
          color: tint,
        ),
      ),
    );
  }
}

class VoiceBar extends StatefulWidget {
  const VoiceBar({super.key, required this.chat, required this.voice});

  final String chat;
  final GroupVoice voice;

  @override
  State<VoiceBar> createState() => _VoiceBarState();
}

class _VoiceBarState extends State<VoiceBar> with WidgetsBindingObserver {
  bool _talking = false;
  bool _released = false;
  bool _live = false;
  Timer? _poll;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _keys(true);
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _poll?.cancel();
    _talk(false);
    _keys(false);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state != AppLifecycleState.resumed) {
      _talk(false);
    }

    _keys(state == AppLifecycleState.resumed);
  }

  void _keys(bool on) {
    Vigil.instance.groupVoiceKeys(on).catchError((Object _) {});
  }

  @override
  void didUpdateWidget(VoiceBar old) {
    super.didUpdateWidget(old);

    if (_talking && !_ready) {
      scheduleMicrotask(() => _talk(false));
    }

    if (_released && !widget.voice.talking) {
      _released = false;
    }
  }

  bool get _ready => widget.voice.joined && !widget.voice.ducked;

  void _talk(bool on) {
    if (_talking == on) {
      return;
    }

    _talking = on;
    _released = !on;
    _live = false;
    _poll?.cancel();
    _poll = null;

    if (on) {
      HapticFeedback.mediumImpact();
      _poll = Timer.periodic(const Duration(milliseconds: 50), (_) => _check());
    }

    if (mounted) {
      setState(() {});
    }

    Vigil.instance.groupVoiceTalk(on).catchError((Object e) {
      if (e is VigilError) {
        notify(e.message);
      }
    });
  }

  Future<void> _check() async {
    final live = await Vigil.instance.groupVoiceReady();

    if (!mounted || !_talking || live == _live) {
      return;
    }

    setState(() => _live = live);

    if (live) {
      _poll?.cancel();
      _poll = null;
    }
  }

  @override
  Widget build(BuildContext context) {
    final voice = widget.voice;
    final busy = voice.busy;
    final talking = _talking || (voice.talking && !_released);
    final live = _live || (voice.ready && !_released && !_talking);

    var fill = Palette.bg3;
    var stroke = Palette.line2;
    var tint = Palette.dim;

    if (_ready) {
      fill = Palette.amberWash;
      stroke = Palette.amberDim;
      tint = Palette.amber;
    }

    if (talking) {
      fill = Palette.amber;
      stroke = Palette.amber2;
      tint = Palette.bg;
    }

    return Padding(
      padding: const EdgeInsets.only(bottom: 10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              const SizedBox(width: 12),
              VoiceMark(voice: voice),
              const SizedBox(width: 8),
              Expanded(
                child: Text(
                  voiceLine(voice, talking: talking),
                  style: Styles.detail.copyWith(
                    fontSize: 12.5,
                    color: busy ? Palette.amber : Palette.dim,
                  ),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                ),
              ),
              _output(voice),
              GlyphBtn(
                voice.muted ? GlyphKind.channelOff : GlyphKind.channel,
                label: voice.muted ? 'Unmute voice' : 'Mute voice',
                color: voice.muted ? Palette.bad : Palette.fg2,
                onTap: () => _mute(!voice.muted),
              ),
              GlyphBtn(
                GlyphKind.close,
                label: 'Turn off voice',
                onTap: () => toggleVoice(widget.chat),
              ),
            ],
          ),
          const SizedBox(height: 4),
          Row(
            children: [
              const SizedBox(width: 6),
              Semantics(
                label: live ? 'Talk now' : 'Do not talk yet',
                child: AnimatedContainer(
                  duration: const Duration(milliseconds: 80),
                  width: 16,
                  height: 16,
                  decoration: BoxDecoration(
                    color: live ? Palette.good : Palette.bad,
                    shape: BoxShape.circle,
                  ),
                ),
              ),
              const SizedBox(width: 12),
              Expanded(child: _button(talking, fill, stroke, tint)),
            ],
          ),
        ],
      ),
    );
  }

  Widget _output(GroupVoice voice) {
    if (voice.output == 'headset') {
      return const GlyphBtn(
        GlyphKind.headset,
        label: 'Voice plays on your headset',
      );
    }

    if (voice.earpiece) {
      return GlyphBtn(
        GlyphKind.earpiece,
        label: 'Voice plays on the earpiece, switch to the speaker',
        onTap: () => _earpiece(false),
      );
    }

    return GlyphBtn(
      GlyphKind.speaker,
      label: 'Voice plays on the speaker, switch to the earpiece',
      onTap: () => _earpiece(true),
    );
  }

  Widget _button(bool talking, Color fill, Color stroke, Color tint) {
    return Semantics(
      button: true,
      label: 'Hold to talk',
      child: Listener(
        onPointerDown: _ready ? (_) => _talk(true) : null,
        onPointerUp: (_) => _talk(false),
        onPointerCancel: (_) => _talk(false),
        child: Container(
          height: 56,
          decoration: BoxDecoration(
            color: fill,
            border: Border.all(color: stroke, width: 1.5),
            borderRadius: BorderRadius.circular(28),
          ),
          child: Row(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Glyph(GlyphKind.mic, size: 22, color: tint),
              const SizedBox(width: 10),
              Text(
                talking ? 'Talking' : 'Hold to talk',
                style: Styles.cardTitle.copyWith(color: tint),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
