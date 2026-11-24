part of 'shell.dart';

final _playing = ValueNotifier<String>('');

class _Conversation extends StatefulWidget {
  const _Conversation({
    super.key,
    required this.chat,
    required this.topic,
    required this.members,
    required this.onMembers,
    required this.onProblem,
  });

  final Chat chat;
  final String topic;
  final bool members;
  final VoidCallback? onMembers;
  final void Function(String) onProblem;

  @override
  State<_Conversation> createState() => _ConversationState();
}

class _ConversationState extends State<_Conversation> {
  final _text = TextEditingController();
  final _focus = FocusNode();
  Message? _reply;
  bool _typing = false;
  int _typed = 0;
  Timer? _idle;
  bool _recording = false;
  int _recorded = 0;
  Timer? _clock;
  List<String>? _cycle;
  int _cycled = 0;
  int _cycleAt = 0;
  String _cycleText = '';

  Chat get _chat => widget.chat;

  @override
  void initState() {
    super.initState();
    _focus.onKeyEvent = _key;
    _text.addListener(() => setState(() {}));
    WidgetsBinding.instance.addPostFrameCallback((_) => _focus.requestFocus());
  }

  @override
  void dispose() {
    _idle?.cancel();
    _clock?.cancel();

    if (_typing) {
      unawaited(
        Desk.instance.typing(_chat, widget.topic, false).catchError((_) {}),
      );
    }

    if (_recording) {
      unawaited(Desk.instance.memoCancel().catchError((_) {}));
    }

    _text.dispose();
    _focus.dispose();
    super.dispose();
  }

  Future<void> _act(Future<void> Function() what) async {
    try {
      await what();
    } on DeskError catch (e) {
      widget.onProblem(e.message);
    }
  }

  KeyEventResult _key(FocusNode node, KeyEvent event) {
    if (event is! KeyDownEvent) {
      return KeyEventResult.ignored;
    }

    final keys = HardwareKeyboard.instance;

    if (event.logicalKey == LogicalKeyboardKey.escape && _reply != null) {
      setState(() => _reply = null);
      return KeyEventResult.handled;
    }

    if (event.logicalKey == LogicalKeyboardKey.tab) {
      return _complete(keys.isShiftPressed)
          ? KeyEventResult.handled
          : KeyEventResult.ignored;
    }

    if (event.logicalKey == LogicalKeyboardKey.keyV && keys.isControlPressed) {
      unawaited(_act(() async {
        await Desk.instance.pasteImage(_chat, topic: widget.topic);
      }));
      return KeyEventResult.ignored;
    }

    if (event.logicalKey != LogicalKeyboardKey.enter &&
        event.logicalKey != LogicalKeyboardKey.numpadEnter) {
      return KeyEventResult.ignored;
    }

    if (!keys.isShiftPressed) {
      return KeyEventResult.ignored;
    }

    final value = _text.value;
    final at = value.selection.isValid
        ? value.selection
        : TextSelection.collapsed(offset: value.text.length);

    _text.value = TextEditingValue(
      text: value.text.replaceRange(at.start, at.end, '\n'),
      selection: TextSelection.collapsed(offset: at.start + 1),
    );

    return KeyEventResult.handled;
  }

  void _changed(String text) {
    final now = DateTime.now().millisecondsSinceEpoch;

    _idle?.cancel();

    if (text.isEmpty) {
      _stopTyping();
      return;
    }

    if (!_typing || now - _typed > 4000) {
      _typing = true;
      _typed = now;
      unawaited(
        Desk.instance.typing(_chat, widget.topic, true).catchError((_) {}),
      );
    }

    _idle = Timer(const Duration(seconds: 5), _stopTyping);
  }

  void _stopTyping() {
    _idle?.cancel();

    if (!_typing) {
      return;
    }

    _typing = false;
    unawaited(
      Desk.instance.typing(_chat, widget.topic, false).catchError((_) {}),
    );
  }

  int get _max {
    final group = _chat.isGroup;
    final topic = widget.topic.isNotEmpty;

    if (group && topic) {
      return messageMax - 31;
    }

    if (group && _reply != null) {
      return messageMax - 23;
    }

    if (group) {
      return messageMax - 14;
    }

    if (topic) {
      return messageMax - 17;
    }

    if (_reply != null) {
      return messageMax - 9;
    }

    return messageMax;
  }

  Future<void> _send() async {
    final text = _text.text.trim();
    if (text.isEmpty) {
      return;
    }

    if (utf8Length(text) > _max) {
      widget.onProblem('That message is too long, keep it under $_max bytes.');
      return;
    }

    try {
      await Desk.instance.send(
        _chat,
        text,
        topic: widget.topic,
        reply: _reply,
      );
      _text.clear();
      _stopTyping();
      setState(() => _reply = null);
    } on DeskError catch (e) {
      widget.onProblem(e.message);
    }
  }

  Future<void> _record() async {
    try {
      await Desk.instance.memoRecord();
    } on DeskError catch (e) {
      widget.onProblem(e.message);
      return;
    }

    setState(() {
      _recording = true;
      _recorded = 0;
    });

    _clock = Timer.periodic(const Duration(seconds: 1), (_) {
      setState(() => _recorded++);

      if (_recorded >= 300) {
        unawaited(_recordDone(true));
      }
    });
  }

  Future<void> _recordDone(bool send) async {
    _clock?.cancel();
    setState(() => _recording = false);

    if (!send) {
      await _act(Desk.instance.memoCancel);
      return;
    }

    await _act(() => Desk.instance.memoSend(_chat, topic: widget.topic));
  }

  void _mention(int start, String peer) {
    final value = _text.value;
    final end = value.selection.baseOffset;
    final name = '@${Desk.instance.mentionName(peer)} ';

    _text.value = TextEditingValue(
      text: value.text.replaceRange(start, end, name),
      selection: TextSelection.collapsed(offset: start + name.length),
    );

    _focus.requestFocus();
  }

  bool _complete(bool back) {
    final cycle = _cycle;

    if (cycle != null && _text.text == _cycleText) {
      final n = cycle.length;
      _cycled = (_cycled + (back ? n - 1 : 1)) % n;
      _cycleTo(cycle[_cycled]);
      return true;
    }

    _cycle = null;

    final mentioning = _mentioning();
    if (mentioning == null) {
      return false;
    }

    final (start, found) = mentioning;

    if (found.length == 1) {
      _mention(start, found.first);
      return true;
    }

    _cycle = found;
    _cycled = back ? found.length - 1 : 0;
    _cycleAt = start;
    _cycleTo(found[_cycled]);

    return true;
  }

  void _cycleTo(String peer) {
    final value = _text.value;
    final end = value.selection.baseOffset;
    final name = '@${Desk.instance.mentionName(peer)} ';

    _cycleText = value.text.replaceRange(_cycleAt, end, name);
    _text.value = TextEditingValue(
      text: _cycleText,
      selection: TextSelection.collapsed(offset: _cycleAt + name.length),
    );
  }

  (int, List<String>)? _mentioning() {
    if (!_chat.isGroup) {
      return null;
    }

    final value = _text.value;
    final at = value.selection.baseOffset;

    if (at < 0 || at > value.text.length) {
      return null;
    }

    final before = value.text.substring(0, at);
    final match = RegExp(r'(?:^|\s)@([^\s@]*)$').firstMatch(before);

    if (match == null) {
      return null;
    }

    final part = match.group(1)!.toLowerCase();
    final desk = Desk.instance;
    final found = [
      for (final m in _chat.members)
        if (m != desk.selfIn(_chat.id) &&
            desk.mentionName(m).toLowerCase().startsWith(part))
          m,
    ];

    if (found.isEmpty) {
      return null;
    }

    return (at - part.length - 1, found);
  }

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final chat = _chat;
    final topic = widget.topic;
    final messages = chat.messages.where((m) => m.topic == topic).toList();
    final typers = desk.typers(chat, topic);
    final pinned = messages.where((m) => m.pinned && !m.gone).toList();
    final cycling = _cycle != null && _text.text == _cycleText;
    final mentioning = cycling ? (_cycleAt, _cycle!) : _mentioning();

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        _Header(
          chat: chat,
          topic: topic,
          members: widget.members,
          onMembers: widget.onMembers,
          act: _act,
        ),
        const ColoredBox(color: Palette.line, child: SizedBox(height: 1)),
        if (pinned.isNotEmpty) _Pinned(chat: chat, pinned: pinned, act: _act),
        Expanded(
          child: SelectionArea(
            child: ListView.builder(
              reverse: true,
              padding: const EdgeInsets.symmetric(vertical: 16),
              itemCount: messages.length,
              itemBuilder: (context, i) {
                final at = messages.length - 1 - i;
                final message = messages[at];
                final prev = at > 0 ? messages[at - 1] : null;
                final day = prev == null || _day(prev.at) != _day(message.at);
                final head =
                    day ||
                    prev.system.isNotEmpty ||
                    prev.author != message.author ||
                    desk.mine(prev) != desk.mine(message) ||
                    message.at - prev.at > 5 * 60 * 1000;

                final line = message.system.isNotEmpty
                    ? _SystemLine(message: message)
                    : _Line(
                        chat: chat,
                        message: message,
                        head: head,
                        act: _act,
                        onReply: () {
                          setState(() => _reply = message);
                          _focus.requestFocus();
                        },
                      );

                if (!day) {
                  return line;
                }

                return Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [_DayLine(at: message.at), line],
                );
              },
            ),
          ),
        ),
        SizedBox(
          height: 20,
          child: typers.isEmpty
              ? null
              : Padding(
                  padding: const EdgeInsets.only(left: 22),
                  child: Text(
                    typers.length == 1
                        ? '${desk.name(typers.first)} is typing…'
                        : '${typers.map(desk.name).join(', ')} are typing…',
                    style: Styles.detail.copyWith(
                      color: Palette.amber,
                      fontStyle: FontStyle.italic,
                    ),
                  ),
                ),
        ),
        if (_reply != null)
          _ReplyBar(
            message: _reply!,
            onCancel: () => setState(() => _reply = null),
          ),
        if (mentioning != null)
          _Mentions(
            members: mentioning.$2,
            current: cycling ? _cycle![_cycled] : null,
            onPick: (m) {
              _cycle = null;
              _mention(mentioning.$1, m);
            },
          ),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 6, 16, 16),
          child: chat.closed
              ? Text(_closedText(chat), style: Styles.rubric)
              : _recording
              ? _Recording(
                  seconds: _recorded,
                  onCancel: () => _recordDone(false),
                  onSend: () => _recordDone(true),
                )
              : Row(
                  crossAxisAlignment: CrossAxisAlignment.end,
                  children: [
                    _Icon(
                      icon: GlyphKind.image,
                      tip: 'Send an image',
                      onTap: () => _act(
                        () => desk.sendFile(chat, image: true, topic: topic),
                      ),
                    ),
                    _Icon(
                      icon: GlyphKind.plus,
                      tip: 'Send a file',
                      onTap: () => _act(
                        () => desk.sendFile(chat, image: false, topic: topic),
                      ),
                    ),
                    const SizedBox(width: 6),
                    Expanded(
                      child: _Field(
                        controller: _text,
                        focus: _focus,
                        hint: _hint(chat, topic),
                        lines: 8,
                        onSubmit: _send,
                        onChanged: _changed,
                      ),
                    ),
                    const SizedBox(width: 6),
                    if (_text.text.trim().isEmpty)
                      _Icon(
                        icon: GlyphKind.mic,
                        tip: 'Record a voice message',
                        onTap: _record,
                      )
                    else
                      _Icon(icon: GlyphKind.send, tip: 'Send', onTap: _send),
                  ],
                ),
        ),
      ],
    );
  }
}

String _hint(Chat chat, String topic) {
  final desk = Desk.instance;
  final name = chat.topics[topic]?.name ?? '';

  if (name.isNotEmpty) {
    return 'Message #$name';
  }

  return 'Message ${desk.title(chat)}';
}

String _closedText(Chat chat) {
  switch (chat.state) {
    case 'left':
      return 'YOU LEFT THIS GROUP';
    case 'removed':
      return 'YOU ARE NO LONGER IN THIS GROUP';
  }

  return 'THIS GROUP WAS ENDED';
}

class _Recording extends StatelessWidget {
  const _Recording({
    required this.seconds,
    required this.onCancel,
    required this.onSend,
  });

  final int seconds;
  final VoidCallback onCancel;
  final VoidCallback onSend;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.fromLTRB(14, 4, 4, 4),
      decoration: BoxDecoration(
        color: Palette.bg,
        border: Border.all(color: Palette.amberDim),
        borderRadius: const BorderRadius.all(Radius.circular(6)),
      ),
      child: Row(
        children: [
          const _Dot(color: Color(0xffff6b6b), size: 9),
          const SizedBox(width: 10),
          Text(
            'Recording ${_duration(seconds * 1000)}',
            style: const TextStyle(color: Palette.fg),
          ),
          const Spacer(),
          _Icon(icon: GlyphKind.close, tip: 'Throw away', onTap: onCancel),
          _Icon(icon: GlyphKind.send, tip: 'Send', onTap: onSend, lit: true),
        ],
      ),
    );
  }
}

class _Mentions extends StatelessWidget {
  const _Mentions({
    required this.members,
    required this.current,
    required this.onPick,
  });

  final List<String> members;
  final String? current;
  final void Function(String) onPick;

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final at = current == null ? 0 : members.indexOf(current!);
    final shown = members.skip(at < 6 ? 0 : at - 5).take(6);

    return Container(
      margin: const EdgeInsets.fromLTRB(16, 0, 16, 4),
      padding: const EdgeInsets.symmetric(vertical: 4),
      decoration: BoxDecoration(
        color: Palette.bg3,
        border: Border.all(color: Palette.line2),
        borderRadius: const BorderRadius.all(Radius.circular(6)),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          for (final m in shown)
            _Clickable(
              onTap: () => onPick(m),
              child: Container(
                color: m == current ? Palette.amberWash : null,
                padding: const EdgeInsets.symmetric(
                  horizontal: 12,
                  vertical: 5,
                ),
                child: Row(
                  children: [
                    _Avatar(peer: m, size: 22),
                    const SizedBox(width: 10),
                    Text(
                      '@${desk.mentionName(m)}',
                      style: const TextStyle(color: Palette.fg),
                    ),
                  ],
                ),
              ),
            ),
        ],
      ),
    );
  }
}

class _Header extends StatelessWidget {
  const _Header({
    required this.chat,
    required this.topic,
    required this.members,
    required this.onMembers,
    required this.act,
  });

  final Chat chat;
  final String topic;
  final bool members;
  final VoidCallback? onMembers;
  final Future<void> Function(Future<void> Function()) act;

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final current = chat.named.where((t) => t.id == topic).firstOrNull;

    var sub = '';
    var color = Palette.dim;
    if (chat.isGroup) {
      final others = chat.members.where((m) => m != desk.selfIn(chat.id));
      final around = others.where(desk.around).length;
      sub = '$around of ${others.length} around';
    } else {
      color = _presence(chat.id) ?? Palette.dim;
      sub = switch (_rank(chat.id)) {
        2 => 'Online',
        1 => 'Around',
        _ => 'Offline',
      };
    }

    final busy = desk.call != null || desk.ringingFor != null;

    return SizedBox(
      height: 56,
      child: Row(
        children: [
          const SizedBox(width: 20),
          Expanded(
            child: _Clickable(
              onTap: () => unawaited(_details(context, chat)),
              child: Row(
                children: [
                  _Hash(topic: current, size: 22),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Column(
                      mainAxisAlignment: MainAxisAlignment.center,
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Row(
                          children: [
                            Flexible(
                              child: Text(
                                current?.name ?? 'Main',
                                style: Styles.barTitle,
                                maxLines: 1,
                                overflow: TextOverflow.ellipsis,
                              ),
                            ),
                            if (!chat.isGroup) ...[
                              const SizedBox(width: 12),
                              _Dot(color: color),
                              const SizedBox(width: 7),
                              Text(
                                sub,
                                style: Styles.detail.copyWith(color: color),
                              ),
                            ],
                          ],
                        ),
                        if (chat.isGroup)
                          Text(
                            sub,
                            style: Styles.detail,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                          ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
          ),
          if (onMembers != null)
            _Icon(
              icon: GlyphKind.group,
              tip: members ? 'Hide members' : 'Show members',
              lit: members,
              onTap: onMembers!,
            ),
          if (!chat.isGroup && !busy)
            _Icon(
              icon: GlyphKind.call,
              tip: 'Call ${desk.title(chat)}',
              onTap: () => act(() => desk.dial(chat.id)),
            ),
          Builder(
            builder: (context) => _Icon(
              icon: GlyphKind.grip,
              tip: 'More',
              onTap: () {
                final box = context.findRenderObject() as RenderBox;
                final at = box.localToGlobal(Offset(0, box.size.height));
                unawaited(_chatMenu(context, chat, at, act));
              },
            ),
          ),
          const SizedBox(width: 12),
        ],
      ),
    );
  }
}

Future<void> _details(BuildContext context, Chat chat) {
  return showDialog<void>(
    context: context,
    builder: (_) => _DetailsDialog(chat: chat),
  );
}

Color? _presence(String peer) => switch (_rank(peer)) {
  2 => Palette.good,
  1 => Palette.amber,
  _ => null,
};

int _rank(String peer) {
  final state = Desk.instance.peers[peer];

  if (state == null) {
    return 0;
  }

  if (state.online) {
    return 2;
  }

  if (state.present) {
    return 1;
  }

  return 0;
}

class _DetailsDialog extends StatelessWidget {
  const _DetailsDialog({required this.chat});

  final Chat chat;

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<int>(
      valueListenable: Desk.instance.changes,
      builder: (context, _, _) => _build(context),
    );
  }

  Widget _build(BuildContext context) {
    final desk = Desk.instance;
    final members = chat.members.toList()
      ..sort((a, b) => desk.name(a).compareTo(desk.name(b)));

    return _Dialog(
      title: desk.title(chat),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          _Detail(label: 'FLOCK', value: chat.flock),
          if (chat.isGroup)
            _Detail(label: 'OWNER', value: chat.owner)
          else
            _Detail(label: 'PEER ID', value: chat.id),
          if (desk.selfIn(chat.id).isNotEmpty)
            _Detail(label: 'YOUR ID', value: desk.selfIn(chat.id)),
          if (members.isNotEmpty) ...[
            const SizedBox(height: 16),
            Text('MEMBERS · ${members.length}', style: Styles.rubric),
            const SizedBox(height: 6),
            ConstrainedBox(
              constraints: const BoxConstraints(maxHeight: 300),
              child: ListView(
                shrinkWrap: true,
                padding: const EdgeInsets.only(right: 16),
                children: [
                  for (final member in members)
                    Padding(
                      padding: const EdgeInsets.symmetric(vertical: 6),
                      child: Row(
                        children: [
                          _Avatar(peer: member, size: 26, presence: true),
                          const SizedBox(width: 10),
                          Expanded(
                            child: Text(
                              desk.name(member),
                              style: const TextStyle(color: Palette.fg),
                              overflow: TextOverflow.ellipsis,
                            ),
                          ),
                          const SizedBox(width: 12),
                          SelectableText(member, style: Styles.detail),
                        ],
                      ),
                    ),
                ],
              ),
            ),
          ],
          const SizedBox(height: 16),
          Row(
            mainAxisAlignment: MainAxisAlignment.end,
            children: [
              _Button(
                label: 'Close',
                ghost: true,
                onTap: () => Navigator.of(context).pop(),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _Detail extends StatelessWidget {
  const _Detail({required this.label, required this.value});

  final String label;
  final String value;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 5),
      child: Row(
        children: [
          Expanded(child: Text(label, style: Styles.rubric)),
          SelectableText(
            value,
            style: Styles.detail.copyWith(color: Palette.fg),
          ),
        ],
      ),
    );
  }
}

class _Members extends StatelessWidget {
  const _Members({required this.chat, required this.onOpen});

  final Chat chat;
  final void Function(Chat) onOpen;

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final members = chat.members.where((m) => m != desk.selfIn(chat.id)).toList()
      ..sort((a, b) {
        final rank = _rank(b).compareTo(_rank(a));

        if (rank != 0) {
          return rank;
        }

        return desk.name(a).compareTo(desk.name(b));
      });

    final around = members.where(desk.around).toList();
    final away = members.where((m) => !desk.around(m)).toList();

    return ColoredBox(
      color: Palette.bg2,
      child: SizedBox(
        width: 220,
        child: ListView(
          padding: const EdgeInsets.symmetric(vertical: 8),
          children: [
            if (around.isNotEmpty) _label('AROUND', around.length),
            for (final member in around) _row(member, 1),
            if (away.isNotEmpty) _label('OFFLINE', away.length),
            for (final member in away) _row(member, 0.5),
          ],
        ),
      ),
    );
  }

  Widget _label(String text, int count) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 12, 16, 6),
      child: Text('$text · $count', style: Styles.rubric),
    );
  }

  Widget _row(String member, double opacity) {
    final desk = Desk.instance;

    return _Clickable(
      onTap: () => onOpen(desk.chat(member)),
      child: Opacity(
        opacity: opacity,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 5),
          child: Row(
            children: [
              _Avatar(peer: member, size: 30, presence: true),
              const SizedBox(width: 10),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      desk.name(member),
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        fontSize: 13.5,
                        height: 1.3,
                        color: Palette.fg,
                      ),
                    ),
                    if (member == chat.owner)
                      const Text('owner', style: Styles.detail),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _Pinned extends StatelessWidget {
  const _Pinned({required this.chat, required this.pinned, required this.act});

  final Chat chat;
  final List<Message> pinned;
  final Future<void> Function(Future<void> Function()) act;

  Future<void> _list(BuildContext context) async {
    final desk = Desk.instance;

    await showDialog<void>(
      context: context,
      builder: (context) => _Dialog(
        title: 'Pinned',
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxHeight: 420),
          child: ListView(
            shrinkWrap: true,
            children: [
              for (final m in pinned.reversed)
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 6),
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      _Avatar(peer: m.author, size: 26),
                      const SizedBox(width: 10),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text(
                              '${desk.name(m.author)} · ${_when(m.at)}',
                              style: Styles.detail,
                            ),
                            Text(
                              m.summary,
                              style: const TextStyle(color: Palette.fg2),
                            ),
                          ],
                        ),
                      ),
                      _Icon(
                        icon: GlyphKind.close,
                        tip: 'Unpin',
                        onTap: () {
                          Navigator.of(context).pop();
                          unawaited(act(() => desk.pin(m, false)));
                        },
                      ),
                    ],
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final last = pinned.last;

    return _Clickable(
      onTap: () => _list(context),
      child: Container(
        color: Palette.bg2,
        padding: const EdgeInsets.fromLTRB(20, 8, 20, 8),
        child: Row(
          children: [
            const Glyph(GlyphKind.pin, size: 15, color: Palette.amber),
            const SizedBox(width: 10),
            Expanded(
              child: Text(
                '${desk.name(last.author)}: ${last.summary}',
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(fontSize: 13, color: Palette.fg2),
              ),
            ),
            if (pinned.length > 1)
              Text('${pinned.length} PINNED', style: Styles.rubric),
          ],
        ),
      ),
    );
  }
}

class _DayLine extends StatelessWidget {
  const _DayLine({required this.at});

  final int at;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(20, 16, 20, 4),
      child: Row(
        children: [
          const Expanded(
            child: ColoredBox(color: Palette.line, child: SizedBox(height: 1)),
          ),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12),
            child: Text(_day(at).toUpperCase(), style: Styles.rubric),
          ),
          const Expanded(
            child: ColoredBox(color: Palette.line, child: SizedBox(height: 1)),
          ),
        ],
      ),
    );
  }
}

class _SystemLine extends StatelessWidget {
  const _SystemLine({required this.message});

  final Message message;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(72, 6, 20, 6),
      child: Text(
        '${_line(message)} · ${_clock(message.at)}',
        style: Styles.detail.copyWith(fontStyle: FontStyle.italic),
      ),
    );
  }
}

class _Line extends StatefulWidget {
  const _Line({
    required this.chat,
    required this.message,
    required this.head,
    required this.onReply,
    required this.act,
  });

  final Chat chat;
  final Message message;
  final bool head;
  final VoidCallback onReply;
  final Future<void> Function(Future<void> Function()) act;

  @override
  State<_Line> createState() => _LineState();
}

class _LineState extends State<_Line> {
  final _bar = OverlayPortalController();
  final _anchor = LayerLink();
  bool _hover = false;
  bool _onBar = false;

  void _point({bool? line, bool? bar}) {
    setState(() {
      _hover = line ?? _hover;
      _onBar = bar ?? _onBar;
    });

    if (_hover || _onBar) {
      _bar.show();
    } else {
      _bar.hide();
    }
  }

  Widget _tools(BuildContext context) {
    final desk = Desk.instance;
    final message = widget.message;

    return Positioned(
      left: 0,
      top: 0,
      child: CompositedTransformFollower(
        link: _anchor,
        showWhenUnlinked: false,
        targetAnchor: Alignment.topRight,
        followerAnchor: Alignment.topRight,
        offset: const Offset(-28, -16),
        child: MouseRegion(
          onEnter: (_) => _point(bar: true),
          onExit: (_) => _point(bar: false),
          child: Container(
            decoration: BoxDecoration(
              color: Palette.bg3,
              border: Border.all(color: Palette.line2),
              borderRadius: const BorderRadius.all(Radius.circular(6)),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                if (!widget.chat.closed && !message.gone)
                  _Icon(
                    icon: GlyphKind.reply,
                    tip: 'Reply',
                    onTap: widget.onReply,
                  ),
                _Icon(
                  icon: GlyphKind.star,
                  tip: message.favorite ? 'Remove from favorites' : 'Favorite',
                  lit: message.favorite,
                  onTap: () => widget.act(
                    () => desk.favorite(message, !message.favorite),
                  ),
                ),
                Builder(
                  builder: (context) => _Icon(
                    icon: GlyphKind.grip,
                    tip: 'More',
                    onTap: () {
                      final box = context.findRenderObject() as RenderBox;
                      unawaited(
                        _more(box.localToGlobal(Offset(0, box.size.height))),
                      );
                    },
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Future<void> _more(Offset at) async {
    final desk = Desk.instance;
    final message = widget.message;
    final mine = desk.mine(message);

    final what = await _popup(context, at, [
      if (!widget.chat.closed && !message.gone) _item('reply', 'Reply'),
      if (!message.data) _item('copy', 'Copy text'),
      if (message.data && message.file.isNotEmpty && !message.gone)
        _item('save', 'Save as…'),
      _item(
        'favorite',
        message.favorite ? 'Remove from favorites' : 'Favorite',
      ),
      _item('pin', message.pinned ? 'Unpin' : 'Pin'),
      if (mine && message.image && !message.gone && message.ref != 0)
        _item('retract', 'Delete image'),
    ]);

    if (what == null || !mounted) {
      return;
    }

    switch (what) {
      case 'reply':
        widget.onReply();
      case 'copy':
        await Clipboard.setData(ClipboardData(text: message.text));
      case 'save':
        await widget.act(() => desk.save(message));
      case 'favorite':
        await widget.act(() => desk.favorite(message, !message.favorite));
      case 'pin':
        await widget.act(() => desk.pin(message, !message.pinned));
      case 'retract':
        final sure = await _confirm(
          context,
          'Delete this image?',
          'Your phone asks everyone it went to to delete it as well.',
          'Delete',
        );
        if (sure) {
          await widget.act(() => desk.retract(widget.chat, message));
        }
    }
  }

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final message = widget.message;
    final mine = desk.mine(message);
    final quote = desk.quoted(widget.chat, message);
    final mention = !mine &&
        message.text.contains('@') &&
        mentions(message.text, desk.selfNames(widget.chat.id)).isNotEmpty;

    var bg = const Color(0x00000000);
    if (mention) {
      bg = Palette.amberWash;
    }
    if (_hover || _onBar) {
      bg = Palette.bg2;
    }

    return MouseRegion(
      onEnter: (_) => _point(line: true),
      onExit: (_) => _point(line: false),
      child: _RightClick(
        onMenu: _more,
        child: OverlayPortal(
          controller: _bar,
          overlayChildBuilder: _tools,
          child: CompositedTransformTarget(
            link: _anchor,
            child: Container(
              decoration: BoxDecoration(
                color: bg,
                border: mention
                    ? const Border(
                        left: BorderSide(color: Palette.amber, width: 2),
                      )
                    : null,
              ),
              padding: EdgeInsets.fromLTRB(20, widget.head ? 12 : 2, 20, 2),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  SizedBox(
                    width: 52,
                    child: widget.head
                        ? Align(
                            alignment: Alignment.topLeft,
                            child: _Avatar(
                              peer: mine ? '00' : message.author,
                              size: 36,
                            ),
                          )
                        : _hover
                        ? Padding(
                            padding: const EdgeInsets.only(top: 3),
                            child: Text(
                              _clock(message.at),
                              style: Styles.detail.copyWith(fontSize: 10),
                            ),
                          )
                        : null,
                  ),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        if (widget.head)
                          Padding(
                            padding: const EdgeInsets.only(bottom: 2),
                            child: Row(
                              children: [
                                Text(
                                  desk.author(message),
                                  style: TextStyle(
                                    fontSize: 14,
                                    fontWeight: FontWeight.w600,
                                    color: mine ? Palette.mine : Palette.fg,
                                  ),
                                ),
                                const SizedBox(width: 8),
                                Text(_when(message.at), style: Styles.detail),
                              ],
                            ),
                          ),
                        if (message.quoteHash.isNotEmpty)
                          _Quote(message: quote, author: message.quoteAuthor),
                        _Body(message: message),
                      ],
                    ),
                  ),
                  if (message.favorite || message.pinned)
                    Padding(
                      padding: const EdgeInsets.only(left: 8, top: 3),
                      child: Glyph(
                        message.pinned ? GlyphKind.pin : GlyphKind.star,
                        size: 12,
                        color: Palette.amberDim,
                      ),
                    ),
                  if (mine) _State(chat: widget.chat, message: message),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _Body extends StatelessWidget {
  const _Body({required this.message});

  final Message message;

  @override
  Widget build(BuildContext context) {
    if (message.gone) {
      return Text(
        message.image ? 'This image was deleted' : 'This was deleted',
        style: Styles.detail.copyWith(fontStyle: FontStyle.italic),
      );
    }

    if (message.image) {
      return _Picture(message: message);
    }

    if (message.voice) {
      return _VoiceNote(message: message);
    }

    if (message.data) {
      return _FileChip(message: message);
    }

    return _Text(text: message.text);
  }
}

class _Text extends StatelessWidget {
  const _Text({required this.text});

  final String text;

  Future<void> _open(BuildContext context, String url) async {
    final uri = Uri.tryParse(url.contains('://') ? url : 'https://$url');

    if (uri == null) {
      return;
    }

    final sure = await _confirm(
      context,
      'Open this link?',
      '${uri.host}\n\n$uri',
      'Open',
    );

    if (sure) {
      await Desk.instance.url(uri.toString());
    }
  }

  @override
  Widget build(BuildContext context) {
    const style = TextStyle(fontSize: 14.5, height: 1.45, color: Palette.fg2);
    final desk = Desk.instance;
    final marks = <(int, int, bool)>[
      for (final l in links(text)) (l.start, l.end, true),
      if (text.contains('@'))
        for (final m in mentions(text, desk.mentionable)) (m.start, m.end, false),
    ]..sort((a, b) => a.$1.compareTo(b.$1));

    if (marks.isEmpty) {
      return Text(text, style: style);
    }

    final spans = <InlineSpan>[];
    var at = 0;

    for (final mark in marks) {
      if (mark.$1 < at) {
        continue;
      }

      if (mark.$1 > at) {
        spans.add(TextSpan(text: text.substring(at, mark.$1)));
      }

      final part = text.substring(mark.$1, mark.$2);

      if (mark.$3) {
        spans.add(
          WidgetSpan(
            alignment: PlaceholderAlignment.baseline,
            baseline: TextBaseline.alphabetic,
            child: _Clickable(
              onTap: () => _open(context, part),
              child: Text(
                part,
                style: style.copyWith(
                  color: Palette.amber,
                  decoration: TextDecoration.underline,
                  decorationColor: Palette.amberDim,
                ),
              ),
            ),
          ),
        );
      } else {
        spans.add(
          TextSpan(
            text: part,
            style: const TextStyle(
              color: Palette.amber2,
              fontWeight: FontWeight.w600,
            ),
          ),
        );
      }

      at = mark.$2;
    }

    if (at < text.length) {
      spans.add(TextSpan(text: text.substring(at)));
    }

    return Text.rich(TextSpan(children: spans), style: style);
  }
}

class _Picture extends StatelessWidget {
  const _Picture({required this.message});

  final Message message;

  @override
  Widget build(BuildContext context) {
    return FutureBuilder<Uint8List?>(
      future: Desk.instance.data(message.file),
      builder: (context, snap) {
        final bytes = snap.data;

        if (bytes == null) {
          return _FileChip(message: message);
        }

        return Padding(
          padding: const EdgeInsets.symmetric(vertical: 4),
          child: MouseRegion(
            cursor: SystemMouseCursors.zoomIn,
            child: GestureDetector(
              onTap: () => showDialog<void>(
                context: context,
                barrierColor: Palette.veil,
                builder: (context) => _Viewer(message: message, bytes: bytes),
              ),
              child: ConstrainedBox(
                constraints: const BoxConstraints(
                  maxWidth: 420,
                  maxHeight: 320,
                ),
                child: ClipRRect(
                  borderRadius: const BorderRadius.all(Radius.circular(6)),
                  child: Image.memory(
                    bytes,
                    fit: BoxFit.contain,
                    errorBuilder: (_, _, _) => _FileChip(message: message),
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

class _Viewer extends StatelessWidget {
  const _Viewer({required this.message, required this.bytes});

  final Message message;
  final Uint8List bytes;

  @override
  Widget build(BuildContext context) {
    return CallbackShortcuts(
      bindings: {
        const SingleActivator(LogicalKeyboardKey.escape): () =>
            Navigator.of(context).pop(),
      },
      child: Focus(
        autofocus: true,
        child: Stack(
          children: [
            Positioned.fill(
              child: GestureDetector(
                onTap: () => Navigator.of(context).pop(),
                child: InteractiveViewer(
                  maxScale: 8,
                  child: Center(child: Image.memory(bytes)),
                ),
              ),
            ),
            Positioned(
              top: 16,
              right: 16,
              child: Row(
                children: [
                  _Icon(
                    icon: GlyphKind.chevron,
                    tip: 'Save as…',
                    boxed: true,
                    onTap: () => Desk.instance.save(message),
                  ),
                  const SizedBox(width: 8),
                  _Icon(
                    icon: GlyphKind.close,
                    tip: 'Close',
                    boxed: true,
                    onTap: () => Navigator.of(context).pop(),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _FileChip extends StatelessWidget {
  const _FileChip({required this.message});

  final Message message;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.symmetric(vertical: 4),
      padding: const EdgeInsets.fromLTRB(12, 8, 6, 8),
      decoration: BoxDecoration(
        color: Palette.bg3,
        border: Border.all(color: Palette.line2),
        borderRadius: const BorderRadius.all(Radius.circular(6)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          const Glyph(GlyphKind.paste, size: 18, color: Palette.amber),
          const SizedBox(width: 10),
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                message.name,
                style: const TextStyle(fontSize: 13.5, color: Palette.fg),
              ),
              Text(_size(message.size), style: Styles.detail),
            ],
          ),
          const SizedBox(width: 10),
          if (message.file.isNotEmpty)
            _Icon(
              icon: GlyphKind.chevron,
              tip: 'Save as…',
              onTap: () => Desk.instance.save(message),
            ),
        ],
      ),
    );
  }
}

const _voiceBars = 40;

class _VoiceNote extends StatefulWidget {
  const _VoiceNote({required this.message});

  final Message message;

  @override
  State<_VoiceNote> createState() => _VoiceNoteState();
}

class _VoiceNoteState extends State<_VoiceNote> {
  int _length = 0;
  int _at = 0;
  double _rate = 1;
  List<double> _bars = List.filled(_voiceBars, 0.4);
  Timer? _poll;

  bool get _on => _playing.value == widget.message.key;

  @override
  void initState() {
    super.initState();
    _playing.addListener(_changed);

    Desk.instance.data(widget.message.file).then((bytes) {
      final shape = voiceShape(bytes, _voiceBars);

      if (mounted) {
        setState(() {
          _length = shape.$1;
          _bars = shape.$2 ?? List.filled(_voiceBars, 0.4);
        });
      }
    }, onError: (_) {});
  }

  @override
  void dispose() {
    _playing.removeListener(_changed);
    _poll?.cancel();
    super.dispose();
  }

  void _changed() {
    if (!_on) {
      _poll?.cancel();
    }

    if (mounted) {
      setState(() {});
    }
  }

  Future<void> _play(int from) async {
    final desk = Desk.instance;
    final key = widget.message.key;

    _playing.value = key;
    _at = from;

    _poll?.cancel();
    _poll = Timer.periodic(const Duration(milliseconds: 100), (_) async {
      final now = await desk.memoPosition().catchError((_) => null);

      if (now != null && mounted && _on) {
        setState(() {
          _at = now.$1;
          _length = now.$2;
        });
      }
    });

    try {
      await desk.memoPlay(widget.message, at: from, rate: _rate);
    } on DeskError catch (e) {
      Desk.instance.problems.value = e.message;
    }

    if (_playing.value == key) {
      _playing.value = '';
      _at = 0;
    }
  }

  Future<void> _toggle() async {
    if (_on) {
      _playing.value = '';
      await Desk.instance.memoStop().catchError((_) {});
      return;
    }

    await _play(_at >= _length ? 0 : _at);
  }

  Future<void> _seek(double where) async {
    final to = (where * _length).round();

    if (_on) {
      setState(() => _at = to);
      await Desk.instance.memoSeek(to).catchError((_) {});
      return;
    }

    await _play(to);
  }

  Future<void> _speed() async {
    final next = _rate == 1
        ? 1.5
        : _rate == 1.5
        ? 2.0
        : 1.0;

    setState(() => _rate = next);

    if (_on) {
      await Desk.instance.memoSpeed(next).catchError((_) {});
    }
  }

  @override
  Widget build(BuildContext context) {
    final played = _length == 0 ? 0.0 : _at / _length;

    return Container(
      margin: const EdgeInsets.symmetric(vertical: 4),
      padding: const EdgeInsets.fromLTRB(4, 4, 10, 4),
      decoration: BoxDecoration(
        color: Palette.bg3,
        border: Border.all(color: Palette.line2),
        borderRadius: const BorderRadius.all(Radius.circular(20)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          _Icon(
            icon: _on ? GlyphKind.stop : GlyphKind.play,
            tip: _on ? 'Stop' : 'Play',
            lit: true,
            onTap: _toggle,
          ),
          const SizedBox(width: 6),
          SizedBox(
            width: 200,
            height: 28,
            child: LayoutBuilder(
              builder: (context, box) => MouseRegion(
                cursor: SystemMouseCursors.click,
                child: GestureDetector(
                  onTapDown: (d) =>
                      _seek((d.localPosition.dx / box.maxWidth).clamp(0, 1)),
                  child: CustomPaint(
                    painter: _Wave(bars: _bars, played: played),
                    size: Size(box.maxWidth, box.maxHeight),
                  ),
                ),
              ),
            ),
          ),
          const SizedBox(width: 10),
          Text(
            _duration(_on || _at > 0 ? _at : _length),
            style: Styles.detail,
          ),
          const SizedBox(width: 8),
          _Clickable(
            onTap: _speed,
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
              decoration: BoxDecoration(
                border: Border.all(color: Palette.line2),
                borderRadius: const BorderRadius.all(Radius.circular(4)),
              ),
              child: Text(
                '${_rate == 1 ? 1 : _rate}×',
                style: Styles.detail.copyWith(fontSize: 11),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _Wave extends CustomPainter {
  _Wave({required this.bars, required this.played});

  final List<double> bars;
  final double played;

  @override
  void paint(Canvas canvas, Size size) {
    final step = size.width / bars.length;
    final on = Paint()..color = Palette.amber;
    final off = Paint()..color = Palette.line2;

    for (var i = 0; i < bars.length; i++) {
      final h = size.height * bars[i];
      final x = i * step + step / 2;
      final rect = Rect.fromCenter(
        center: Offset(x, size.height / 2),
        width: step * 0.55,
        height: h,
      );

      canvas.drawRRect(
        RRect.fromRectAndRadius(rect, const Radius.circular(1.5)),
        (i + 0.5) / bars.length <= played ? on : off,
      );
    }
  }

  @override
  bool shouldRepaint(_Wave old) => old.played != played || old.bars != bars;
}

class _Quote extends StatelessWidget {
  const _Quote({required this.message, required this.author});

  final Message? message;
  final String author;

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final quoted = message;

    var text = 'a message that is not here';
    var who = desk.name(author);

    if (quoted != null) {
      who = desk.author(quoted);
      text = quoted.summary;
    }

    return Container(
      margin: const EdgeInsets.only(bottom: 4),
      padding: const EdgeInsets.only(left: 10),
      decoration: const BoxDecoration(
        border: Border(left: BorderSide(color: Palette.amberDim, width: 2)),
      ),
      child: Text.rich(
        TextSpan(
          children: [
            TextSpan(
              text: '$who  ',
              style: const TextStyle(
                fontWeight: FontWeight.w600,
                color: Palette.fg2,
              ),
            ),
            TextSpan(text: text),
          ],
        ),
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
        style: const TextStyle(fontSize: 12.5, color: Palette.dim),
      ),
    );
  }
}

class _State extends StatelessWidget {
  const _State({required this.chat, required this.message});

  final Chat chat;
  final Message message;

  @override
  Widget build(BuildContext context) {
    String text;
    String tip;
    var color = Palette.dim;

    if (chat.isGroup && message.to.isNotEmpty) {
      text = '${message.got.length}/${message.to.length}';
      tip = 'Delivered to ${message.got.length} of ${message.to.length}';
      if (message.got.length >= message.to.length) {
        color = Palette.good;
      }
    } else {
      switch (message.delivery) {
        case Delivery.queued:
          text = '○';
          tip = 'Waiting to go out';
        case Delivery.sent:
          text = '✓';
          tip = 'Sent';
        case Delivery.delivered:
          text = '✓✓';
          tip = 'Delivered';
          color = Palette.good;
        case Delivery.failed:
          text = '✕';
          tip = 'Not delivered';
      }
    }

    return Tooltip(
      message: tip,
      child: Padding(
        padding: const EdgeInsets.only(left: 12, top: 2),
        child: Text(text, style: Styles.detail.copyWith(color: color)),
      ),
    );
  }
}

class _ReplyBar extends StatelessWidget {
  const _ReplyBar({required this.message, required this.onCancel});

  final Message message;
  final VoidCallback onCancel;

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;

    return Container(
      color: Palette.bg2,
      padding: const EdgeInsets.fromLTRB(20, 8, 12, 8),
      child: Row(
        children: [
          const Glyph(GlyphKind.reply, size: 16, color: Palette.amber),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              'Replying to ${desk.author(message)}: '
              '${message.summary}',
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: const TextStyle(fontSize: 13, color: Palette.fg2),
            ),
          ),
          _Icon(icon: GlyphKind.close, tip: 'Cancel', onTap: onCancel),
        ],
      ),
    );
  }
}

class _CallPanel extends StatefulWidget {
  const _CallPanel({required this.act});

  final Future<void> Function(Future<void> Function()) act;

  @override
  State<_CallPanel> createState() => _CallPanelState();
}

class _CallPanelState extends State<_CallPanel> {
  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final ringing = desk.ringingFor;
    final call = desk.call;

    if (ringing != null) {
      return _card(
        peer: ringing,
        text: 'Is calling you',
        buttons: [
          _Button(
            label: 'Decline',
            ghost: true,
            onTap: () => widget.act(() => desk.decline(ringing)),
          ),
          const SizedBox(width: 10),
          _Button(
            label: 'Answer',
            color: Palette.good,
            onTap: () => widget.act(() => desk.answer(ringing)),
          ),
        ],
      );
    }

    if (call == null) {
      final ended = desk.ended;

      if (ended == null) {
        return const SizedBox.shrink();
      }

      return _card(peer: ended.peer, text: ended.text);
    }

    if (!call.relay) {
      return _card(peer: call.peer, text: 'In a call on your phone');
    }

    final muted = desk.voice['muted'] as bool? ?? false;
    final uptime = desk.voice['uptime'] as int? ?? 0;

    var text = 'Connecting…';
    if (call.state == 3) {
      text = _duration(uptime * 1000);
    } else if (call.outgoing) {
      text = call.state == 2 ? 'Ringing…' : 'Calling…';
    }

    return _card(
      peer: call.peer,
      text: text,
      buttons: [
        _Icon(
          icon: GlyphKind.mute,
          tip: muted ? 'Unmute' : 'Mute',
          boxed: true,
          lit: muted,
          onTap: () => widget.act(() => desk.muteCall(!muted, call.peer)),
        ),
        const SizedBox(width: 10),
        _Button(
          label: 'Hang up',
          color: const Color(0xffff6b6b),
          onTap: () => widget.act(() => desk.hangup(call.peer)),
        ),
      ],
    );
  }

  Widget _card({
    required String peer,
    required String text,
    List<Widget> buttons = const [],
  }) {
    final desk = Desk.instance;

    return Container(
      width: 300,
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: Palette.bg3,
        border: Border.all(color: Palette.amberDim),
        borderRadius: const BorderRadius.all(Radius.circular(8)),
        boxShadow: const [
          BoxShadow(color: Color(0x80000000), blurRadius: 24),
        ],
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              _Avatar(peer: peer, size: 40),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      desk.name(peer),
                      style: const TextStyle(
                        fontSize: 15,
                        fontWeight: FontWeight.w600,
                        color: Palette.fg,
                      ),
                    ),
                    Text(text, style: Styles.detail),
                  ],
                ),
              ),
            ],
          ),
          if (buttons.isNotEmpty) ...[
            const SizedBox(height: 14),
            Row(mainAxisAlignment: MainAxisAlignment.end, children: buttons),
          ],
        ],
      ),
    );
  }
}
