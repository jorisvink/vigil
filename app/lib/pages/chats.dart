import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:image_picker/image_picker.dart';

import '../app.dart';
import '../vigil.dart';
import '../theme.dart';
import '../widgets/buttons.dart';
import '../widgets/marks.dart';
import '../widgets/ui.dart';
import 'call.dart';

const _messageMax = 512;
const _voiceMax = Duration(minutes: 5);
const _voiceMin = Duration(milliseconds: 700);
const _voiceBars = 32;

final _playing = ValueNotifier<String?>(null);

(Liveness, String, Color) _link(Status status, String peer) {
  if (!status.gospel) {
    return (Liveness.off, 'GOSPEL IS OFF', Palette.dim);
  }

  if (isGroup(peer)) {
    return _groupLink(status, peer);
  }

  final chat = status.chat?.peer(peer);

  switch (chat?.link) {
    case 'p2p':
      return (Liveness.online, 'LINKED · P2P', Palette.good);
    case 'relay':
      return (Liveness.online, 'LINKED · VIA CATHEDRAL', Palette.good);
    case 'connecting':
      return (Liveness.pending, 'CONNECTING', Palette.amber);
  }

  if (_reachable(status, peer)) {
    return (Liveness.off, 'AROUND · NO LINK', Palette.fg2);
  }

  if ((chat?.queued ?? 0) > 0) {
    return (Liveness.off, 'WAITING FOR PEER', Palette.dim);
  }

  return (Liveness.off, 'OFFLINE', Palette.dim);
}

(Liveness, String, Color) _groupLink(Status status, String chat) {
  final group = status.chat?.group;

  if (status.chats.group != chat) {
    return (Liveness.off, 'NOT JOINED', Palette.dim);
  }

  if (status.chat == null) {
    return (Liveness.off, 'OFFLINE', Palette.dim);
  }

  if (group == null || group.chat != chat || !group.alive) {
    return (Liveness.pending, 'JOINING', Palette.amber);
  }

  final online = group.members.where((m) => m.online).toList();

  if (online.length == 1) {
    return (Liveness.online, '1 MEMBER ONLINE', Palette.good);
  }

  if (online.isNotEmpty) {
    return (Liveness.online, '${online.length} MEMBERS ONLINE', Palette.good);
  }

  if (group.members.isNotEmpty) {
    return (Liveness.pending, 'CONNECTING', Palette.amber);
  }

  return (Liveness.off, 'NOBODY ELSE HERE', Palette.fg2);
}

Widget chatDot(Status status, String peer, {double size = 8}) {
  final state = _link(status, peer).$1;

  if (state == Liveness.off && _reachable(status, peer)) {
    return AroundDot(size: size);
  }

  return StateDot(state, size: size);
}

bool _reachable(Status status, String peer) =>
    (status.call?.peers.contains(peer) ?? false) ||
    (status.chat?.peer(peer)?.present ?? false);

String sender(ChatMessage message) {
  final vigil = Vigil.instance;
  final from = message.from;

  if (vigil.names.containsKey(from) || message.nick.isEmpty) {
    return vigil.name(from);
  }

  return message.nick;
}

String memberLine(ChatMessage message) {
  if (message.kind == 'joined') {
    return '${sender(message)} has joined';
  }

  return '${sender(message)} has left';
}

String _summary(ChatMessage message) {
  if (message.data.isEmpty) {
    return message.text;
  }

  if (message.media == 'image') {
    return 'Image';
  }

  if (message.media == 'voice') {
    return 'Voice message';
  }

  return message.name;
}

String _preview(ChatMessage? last) {
  if (last == null) {
    return 'No messages';
  }

  if (last.kind.isNotEmpty) {
    return memberLine(last);
  }

  final text = _summary(last);

  if (last.incoming) {
    return text;
  }

  return 'You: $text';
}

Future<bool> removeChat(String peer) async {
  final vigil = Vigil.instance;

  try {
    if (vigil.status.chats.group == peer) {
      await vigil.joinGroup('');
    }

    await vigil.deleteChat(peer);
  } on VigilError catch (e) {
    notify(e.message);
    return false;
  }

  return true;
}

String ownPeer() {
  final voice = Vigil.instance.tunnels.where((t) => t.summary.voice);
  final spi = voice.firstOrNull?.summary.spi;
  if (spi == null) {
    return '';
  }

  if (spi.length < 4) {
    return '';
  }

  return spi.substring(2);
}

String _clock(DateTime time) =>
    '${time.hour.toString().padLeft(2, '0')}:'
    '${time.minute.toString().padLeft(2, '0')}';

String _day(DateTime time) {
  const months = [
    'Jan',
    'Feb',
    'Mar',
    'Apr',
    'May',
    'Jun',
    'Jul',
    'Aug',
    'Sep',
    'Oct',
    'Nov',
    'Dec',
  ];

  final now = DateTime.now();
  final today = DateTime(now.year, now.month, now.day);
  final day = DateTime(time.year, time.month, time.day);
  final ago = today.difference(day).inDays;

  if (ago == 0) {
    return 'Today';
  }

  if (ago == 1) {
    return 'Yesterday';
  }

  final date = '${time.day} ${months[time.month - 1]}';
  if (time.year == now.year) {
    return date;
  }

  return '$date ${time.year}';
}

String _when(DateTime time) {
  final day = _day(time);
  if (day == 'Today') {
    return _clock(time);
  }

  return day;
}

Future<void> newChat(Status status) async {
  final context = navigatorKey.currentContext;
  if (context == null) {
    return;
  }

  final vigil = Vigil.instance;
  final own = ownPeer();
  final peers = <String>{
    ...?status.call?.peers,
    ...?status.chat?.peers.where((p) => p.present).map((p) => p.id),
    ...vigil.names.keys.where((p) => !isGroup(p)),
  }.where((p) => p != own).toList()..sort();

  final peer = await sheet<String>(
    context,
    (context) => Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        sheetTitle('Chat with'),
        const SizedBox(height: 12),
        Padding(
          padding: const EdgeInsets.only(bottom: 8),
          child: Panel(
            padding: const EdgeInsets.fromLTRB(16, 12, 12, 12),
            onTap: () => Navigator.of(context).pop(''),
            child: Row(
              children: [
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        status.chats.group == null
                            ? 'Join a group'
                            : 'Swap group',
                        style: Styles.cardTitle,
                      ),
                      Text(
                        status.chats.group == null
                            ? 'Everyone who joins the same group id'
                            : 'Leaves ${vigil.name(status.chats.group!)} '
                                  'for another group',
                        style: Styles.detail,
                        overflow: TextOverflow.ellipsis,
                      ),
                    ],
                  ),
                ),
                const Glyph(GlyphKind.plus, color: Palette.amber),
              ],
            ),
          ),
        ),
        if (peers.isEmpty)
          const Text(
            'Nobody is around right now, peers show up here once they '
            'are online.',
            style: Styles.body,
          ),
        for (final id in peers)
          Padding(
            padding: const EdgeInsets.only(bottom: 8),
            child: Panel(
              padding: const EdgeInsets.fromLTRB(16, 12, 12, 12),
              onTap: () => Navigator.of(context).pop(id),
              child: Row(
                children: [
                  chatDot(status, id),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          vigil.name(id),
                          style: Styles.cardTitle,
                          overflow: TextOverflow.ellipsis,
                        ),
                        if (vigil.name(id) != 'Peer $id')
                          Text('Peer $id', style: Styles.detail),
                      ],
                    ),
                  ),
                  const Glyph(GlyphKind.chat, color: Palette.amber),
                ],
              ),
            ),
          ),
      ],
    ),
  );

  if (peer == '') {
    await joinGroup();
  } else if (peer != null) {
    await push(ChatPage(peer: peer));
  }
}

class _Swipe extends StatefulWidget {
  const _Swipe({
    required this.label,
    required this.onAction,
    required this.child,
  });

  final String label;
  final VoidCallback onAction;
  final Widget child;

  @override
  State<_Swipe> createState() => _SwipeState();
}

class _SwipeState extends State<_Swipe> {
  static const _width = 96.0;

  double _offset = 0;
  bool _dragging = false;

  void _drag(DragUpdateDetails details) {
    setState(() {
      _dragging = true;
      _offset = (_offset + details.delta.dx).clamp(0, _width);
    });
  }

  void _release(DragEndDetails details) {
    final velocity = details.primaryVelocity ?? 0;

    setState(() {
      _dragging = false;
      _offset = _snap(velocity);
    });
  }

  double _snap(double velocity) {
    if (velocity > 300) {
      return _width;
    }

    if (velocity < -300) {
      return 0;
    }

    if (_offset > _width / 2) {
      return _width;
    }

    return 0;
  }

  void _close() {
    setState(() => _offset = 0);
  }

  @override
  Widget build(BuildContext context) {
    final open = _offset > 0 && !_dragging;

    return GestureDetector(
      onHorizontalDragUpdate: _drag,
      onHorizontalDragEnd: _release,
      child: ClipRect(
        child: Stack(
          children: [
            Positioned(
              left: 0,
              top: 0,
              bottom: 0,
              width: _width,
              child: Semantics(
                button: true,
                label: widget.label,
                child: GestureDetector(
                  onTap: () {
                    _close();
                    widget.onAction();
                  },
                  child: Container(
                    color: Palette.amber,
                    alignment: Alignment.center,
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        const Glyph(
                          GlyphKind.close,
                          size: 18,
                          color: Palette.bg,
                        ),
                        const SizedBox(height: 4),
                        Text(
                          'DELETE',
                          style: Styles.rubric.copyWith(color: Palette.bg),
                        ),
                      ],
                    ),
                  ),
                ),
              ),
            ),
            AnimatedContainer(
              duration: _dragging
                  ? Duration.zero
                  : const Duration(milliseconds: 160),
              curve: Curves.easeOut,
              transform: Matrix4.translationValues(_offset, 0, 0),
              child: GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTap: open ? _close : null,
                child: AbsorbPointer(absorbing: open, child: widget.child),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class ChatRow extends StatelessWidget {
  const ChatRow({super.key, required this.chat, required this.status});

  final ChatSummary chat;
  final Status status;

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;
    final last = chat.last;
    final typing = status.chat?.peer(chat.peer)?.typing ?? false;

    return _Swipe(
      label: 'Delete chat with ${vigil.name(chat.peer)}',
      onAction: () => removeChat(chat.peer),
      child: _row(vigil, last, typing),
    );
  }

  Widget _row(Vigil vigil, ChatMessage? last, bool typing) {
    return Panel(
      accent: chat.unread > 0,
      padding: const EdgeInsets.fromLTRB(16, 14, 14, 14),
      onTap: () => push(ChatPage(peer: chat.peer)),
      child: Row(
        children: [
          chatDot(status, chat.peer),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  vigil.name(chat.peer),
                  style: Styles.cardTitle,
                  overflow: TextOverflow.ellipsis,
                ),
                const SizedBox(height: 3),
                Text(
                  typing ? 'typing…' : _preview(last),
                  style: Styles.detail.copyWith(
                    color: typing
                        ? Palette.amber
                        : chat.unread > 0
                        ? Palette.fg
                        : Palette.dim,
                  ),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                ),
              ],
            ),
          ),
          const SizedBox(width: 12),
          Column(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              Text(
                last == null ? '' : _when(last.time),
                style: Styles.detail.copyWith(fontSize: 11),
              ),
              const SizedBox(height: 6),
              if (chat.unread > 0)
                Container(
                  constraints: const BoxConstraints(minWidth: 20),
                  padding: const EdgeInsets.symmetric(
                    horizontal: 6,
                    vertical: 3,
                  ),
                  color: Palette.amber,
                  child: Text(
                    '${chat.unread}',
                    textAlign: TextAlign.center,
                    style: Styles.detail.copyWith(
                      color: Palette.bg,
                      fontSize: 11,
                      height: 1,
                    ),
                  ),
                )
              else
                const SizedBox(height: 17),
            ],
          ),
        ],
      ),
    );
  }
}

class ChatPage extends StatefulWidget {
  const ChatPage({super.key, required this.peer});

  final String peer;

  @override
  State<ChatPage> createState() => _ChatPageState();
}

class _ChatPageState extends State<ChatPage> with WidgetsBindingObserver {
  final _input = TextEditingController();
  StreamSubscription<Status>? _updates;
  ChatHistory? _history;
  String? _error;
  Timer? _idle;
  bool _typing = false;
  bool _sending = false;
  bool _held = false;
  bool _discard = false;
  DateTime? _recording;
  Timer? _ticker;
  int _version = -1;

  String get peer => widget.peer;

  String? _typers(Status status) {
    final vigil = Vigil.instance;

    if (!isGroup(peer)) {
      if (status.chat?.peer(peer)?.typing ?? false) {
        return '';
      }

      return null;
    }

    final group = status.chat?.group;

    if (group == null || group.chat != peer) {
      return null;
    }

    final names = [
      for (final m in group.members)
        if (m.online && m.typing)
          if (vigil.names.containsKey(m.id) || m.nick.isEmpty)
            vigil.name(m.id)
          else
            m.nick,
    ];

    if (names.isEmpty) {
      return null;
    }

    return names.join(', ');
  }

  @override
  void initState() {
    super.initState();

    WidgetsBinding.instance.addObserver(this);

    _updates = Vigil.instance.statuses.listen((status) {
      if (status.chats.version != _version) {
        _load();
      }
    });

    _input.addListener(_changed);
    unawaited(Vigil.instance.openChat(peer, true).catchError((Object _) {}));
    _load();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _updates?.cancel();
    _idle?.cancel();
    _ticker?.cancel();
    _input.dispose();

    final vigil = Vigil.instance;
    if (_recording != null) {
      unawaited(vigil.cancelVoice().catchError((Object _) {}));
    }

    if (_playing.value != null) {
      unawaited(vigil.stopVoice().catchError((Object _) {}));
    }

    unawaited(vigil.typing(peer, false).catchError((Object _) {}));
    unawaited(vigil.openChat(peer, false).catchError((Object _) {}));

    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      unawaited(Vigil.instance.openChat(peer, true).catchError((Object _) {}));
      _load();
    }
  }

  Future<void> _load() async {
    final vigil = Vigil.instance;
    final state = WidgetsBinding.instance.lifecycleState;

    if (state != null && state != AppLifecycleState.resumed) {
      return;
    }

    _version = vigil.status.chats.version;

    try {
      final history = await vigil.chat(peer);
      if (history.unread > 0) {
        await vigil.read(peer);
      }

      if (mounted) {
        setState(() {
          _history = history;
          _error = null;
        });
      }
    } on VigilError catch (e) {
      if (mounted) {
        setState(() => _error = e.message);
      }
    }
  }

  void _changed() {
    final active = _input.text.trim().isNotEmpty;

    _idle?.cancel();
    if (active) {
      _idle = Timer(const Duration(seconds: 5), () => _type(false));
    }

    _type(active);
  }

  void _type(bool on) {
    if (on == _typing) {
      return;
    }

    _typing = on;
    unawaited(Vigil.instance.typing(peer, on).catchError((Object _) {}));
  }

  Future<void> _send() async {
    final text = _input.text;
    if (text.trim().isEmpty || _sending) {
      return;
    }

    setState(() => _sending = true);

    try {
      await Vigil.instance.send(peer, text);
      _input.clear();
      _type(false);
      await _load();
    } on VigilError catch (e) {
      notify(e.message);
    } finally {
      if (mounted) {
        setState(() => _sending = false);
      }
    }
  }

  Future<void> _attach() async {
    if (_sending) {
      return;
    }

    final choice = await sheet<String>(
      context,
      (context) => Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          sheetTitle('Send'),
          const SizedBox(height: 16),
          BtnPair(
            first: Btn(
              label: 'Camera',
              glyph: GlyphKind.scan,
              expand: true,
              onTap: () => Navigator.of(context).pop('camera'),
            ),
            second: Btn(
              label: 'Photos',
              glyph: GlyphKind.image,
              kind: BtnKind.secondary,
              expand: true,
              onTap: () => Navigator.of(context).pop('photos'),
            ),
          ),
          const SizedBox(height: 10),
          Btn(
            label: 'File',
            glyph: GlyphKind.paste,
            kind: BtnKind.secondary,
            expand: true,
            onTap: () => Navigator.of(context).pop('file'),
          ),
        ],
      ),
    );

    if (choice == null) {
      return;
    }

    if (choice == 'file') {
      await _sendWith(() => Vigil.instance.sendFile(peer));
      return;
    }

    final picked = await ImagePicker().pickImage(
      source: choice == 'camera' ? ImageSource.camera : ImageSource.gallery,
    );

    if (picked == null) {
      return;
    }

    await _sendWith(() => Vigil.instance.sendImage(peer, picked.path));
  }

  Future<void> _hold() async {
    if (_sending) {
      return;
    }

    if (_held || _recording != null) {
      return;
    }

    _held = true;
    _discard = false;

    try {
      await Vigil.instance.recordVoice(peer);
    } on VigilError catch (e) {
      _held = false;
      notify(e.message);
      return;
    }

    if (!mounted || !_held) {
      await Vigil.instance.cancelVoice().catchError((Object _) {});
      if (mounted) {
        notify('Hold to record a voice message', warn: false);
      }
      return;
    }

    unawaited(HapticFeedback.mediumImpact());

    setState(() => _recording = DateTime.now());
    _ticker = Timer.periodic(const Duration(milliseconds: 250), (_) {
      final started = _recording;

      if (started != null && DateTime.now().difference(started) >= _voiceMax) {
        unawaited(_release());
        return;
      }

      setState(() {});
    });
  }

  void _slide(Offset offset) {
    final discard = offset.dx < -72;

    if (_recording != null && discard != _discard) {
      unawaited(HapticFeedback.selectionClick());
      setState(() => _discard = discard);
    }
  }

  Future<void> _release({bool cancel = false}) async {
    final started = _recording;

    _held = false;
    if (started == null) {
      return;
    }

    _ticker?.cancel();
    _ticker = null;

    final short = DateTime.now().difference(started) < _voiceMin;
    final discard = cancel || _discard || short;

    setState(() {
      _recording = null;
      _discard = false;
    });

    if (discard) {
      await Vigil.instance.cancelVoice().catchError((Object _) {});
      if (short && !cancel) {
        notify('Hold to record a voice message', warn: false);
      }
      return;
    }

    await _sendWith(() => Vigil.instance.sendVoice(peer));
  }

  Future<void> _sendWith(Future<void> Function() send) async {
    setState(() => _sending = true);

    try {
      await send();
      await _load();
    } on VigilError catch (e) {
      notify(e.message);
    } finally {
      if (mounted) {
        setState(() => _sending = false);
      }
    }
  }

  Future<void> _delete() async {
    final vigil = Vigil.instance;
    final joined = vigil.status.chats.group == peer;

    if (!await confirm(
      title: 'Delete chat',
      text: isGroup(peer)
          ? 'This wipes the messages of ${vigil.name(peer)} from this device'
                '${joined ? ', and leaves the group' : ''}.'
          : 'This wipes the chat with ${vigil.name(peer)} from this device, '
                'messages that were not delivered yet are dropped.',
      action: 'Delete',
    )) {
      return;
    }

    if (await removeChat(peer) && mounted) {
      Navigator.of(context).pop();
    }
  }

  Future<void> _members() async {
    final vigil = Vigil.instance;

    final action = await sheet<String>(
      context,
      (context) => StreamBuilder<Status>(
        stream: vigil.statuses,
        initialData: vigil.status,
        builder: (context, snapshot) {
          final status = snapshot.data!;
          final group = status.chat?.group;
          final joined = status.chats.group == peer && group?.chat == peer;
          final members = joined ? group!.members : <ChatMember>[];

          members.sort((a, b) {
            if (a.online && !b.online) {
              return -1;
            }

            if (!a.online && b.online) {
              return 1;
            }

            return a.id.compareTo(b.id);
          });

          return Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              sheetTitle(vigil.name(peer)),
              const SizedBox(height: 6),
              Text(
                'GROUP ${peer.substring(1)} · '
                '${members.where((m) => m.online).length} OF '
                '${members.length} ONLINE',
                style: Styles.rubric,
              ),
              const SizedBox(height: 12),
              if (members.isEmpty)
                Text(
                  joined
                      ? 'Nobody else is in this group right now.'
                      : 'You are not in this group.',
                  style: Styles.body,
                ),
              ConstrainedBox(
                constraints: BoxConstraints(
                  maxHeight: MediaQuery.sizeOf(context).height * 0.5,
                ),
                child: SingleChildScrollView(
                  child: Column(
                    children: [
                      for (final member in members)
                        Padding(
                          padding: const EdgeInsets.only(bottom: 8),
                          child: _Member(
                            member: member,
                            onTap: () => Navigator.of(context).pop(member.id),
                          ),
                        ),
                    ],
                  ),
                ),
              ),
              const SizedBox(height: 8),
              Btn(
                label: 'Rename group',
                kind: BtnKind.secondary,
                expand: true,
                onTap: () => Navigator.of(context).pop(''),
              ),
            ],
          );
        },
      ),
    );

    if (action == '') {
      await renamePeer(peer);
    } else if (action != null && action != ownPeer()) {
      await push(ChatPage(peer: action));
    }
  }

  Future<void> _membership(bool join) async {
    try {
      await Vigil.instance.joinGroup(join ? peer.substring(1) : '');
    } on VigilError catch (e) {
      notify(e.message);
    }
  }

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;
    final group = isGroup(peer);

    return PopScope(
      onPopInvokedWithResult: (didPop, result) {
        FocusManager.instance.primaryFocus?.unfocus();
      },
      child: StreamBuilder<Status>(
        stream: vigil.statuses,
        initialData: vigil.status,
        builder: (context, snapshot) {
          final status = snapshot.data!;
          final joined = status.chats.group == peer;
          final callable = status.call?.peers.contains(peer) ?? false;
          final messages = _history?.messages ?? const <ChatMessage>[];
          final keyboard = MediaQuery.viewInsetsOf(context).bottom;
          final inset = keyboard > 0
              ? 0.0
              : MediaQuery.paddingOf(context).bottom;

          final (_, presence, color) = _link(status, peer);
          final typing = _typers(status);
          final extra = typing == null ? 0 : 1;

          return DotGrid(
            child: Padding(
              padding: EdgeInsets.only(bottom: keyboard),
              child: SafeArea(
                bottom: false,
                child: Column(
                  children: [
                    SizedBox(
                      height: 56,
                      child: Row(
                        children: [
                          const SizedBox(width: Metrics.edge - 12),
                          GlyphBtn(
                            GlyphKind.back,
                            label: 'Back',
                            onTap: () => Navigator.of(context).maybePop(),
                          ),
                          const SizedBox(width: 2),
                          Expanded(
                            child: GestureDetector(
                              behavior: HitTestBehavior.opaque,
                              onTap: () async {
                                if (group) {
                                  await _members();
                                } else {
                                  await renamePeer(peer);
                                }

                                if (mounted) {
                                  setState(() {});
                                }
                              },
                              child: Column(
                                mainAxisAlignment: MainAxisAlignment.center,
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  Row(
                                    children: [
                                      chatDot(status, peer, size: 6),
                                      const SizedBox(width: 8),
                                      Flexible(
                                        child: Text(
                                          presence,
                                          style: Styles.rubric.copyWith(
                                            color: color,
                                          ),
                                          overflow: TextOverflow.ellipsis,
                                        ),
                                      ),
                                    ],
                                  ),
                                  const SizedBox(height: 2),
                                  Text(
                                    vigil.name(peer),
                                    style: Styles.barTitle,
                                    overflow: TextOverflow.ellipsis,
                                  ),
                                ],
                              ),
                            ),
                          ),
                          if (group)
                            GlyphBtn(
                              joined ? GlyphKind.hangup : GlyphKind.plus,
                              label: joined ? 'Leave group' : 'Join group',
                              onTap: () => _membership(!joined),
                            ),
                          if (callable && status.phone == null && videoCalls)
                            GlyphBtn(
                              GlyphKind.video,
                              label: 'Video call ${vigil.name(peer)}',
                              onTap: () => callPeer(peer, video: true),
                            ),
                          if (callable && status.phone == null)
                            GlyphBtn(
                              GlyphKind.call,
                              label: 'Call ${vigil.name(peer)}',
                              onTap: () => callPeer(peer),
                            ),
                          GlyphBtn(
                            GlyphKind.close,
                            label: 'Delete chat',
                            onTap: _delete,
                          ),
                          const SizedBox(width: Metrics.edge - 10),
                        ],
                      ),
                    ),
                    const Line(),
                    if (_error != null || _history?.warning != null)
                      Padding(
                        padding: const EdgeInsets.fromLTRB(
                          Metrics.edge,
                          Metrics.gap,
                          Metrics.edge,
                          0,
                        ),
                        child: Notice(
                          text: _error ?? _history!.warning!,
                          kind: NoticeKind.warn,
                        ),
                      ),
                    Expanded(
                      child: messages.isEmpty && typing == null
                          ? Center(
                              child: Padding(
                                padding: const EdgeInsets.all(Metrics.edge * 2),
                                child: Text(
                                  _history == null
                                      ? ''
                                      : group
                                      ? 'Messages go to the members of '
                                            '${vigil.name(peer)} that are '
                                            'around right now.'
                                      : 'Say something to ${vigil.name(peer)}, '
                                            'it is held until they are around.',
                                  textAlign: TextAlign.center,
                                  style: Styles.body.copyWith(
                                    color: Palette.dim,
                                  ),
                                ),
                              ),
                            )
                          : ListView.builder(
                              reverse: true,
                              padding: const EdgeInsets.fromLTRB(
                                Metrics.edge,
                                12,
                                Metrics.edge,
                                12,
                              ),
                              itemCount: messages.length + extra,
                              itemBuilder: (context, index) {
                                if (index < extra) {
                                  return _Typing(
                                    sender: typing!.isEmpty ? null : typing,
                                  );
                                }

                                final at = messages.length - 1 - index + extra;
                                final message = messages[at];
                                final previous = at > 0
                                    ? messages[at - 1]
                                    : null;
                                final day = _day(message.time);

                                return Column(
                                  children: [
                                    if (previous == null ||
                                        _day(previous.time) != day)
                                      Padding(
                                        padding: const EdgeInsets.symmetric(
                                          vertical: 14,
                                        ),
                                        child: Text(
                                          day.toUpperCase(),
                                          style: Styles.rubric,
                                        ),
                                      ),
                                    if (message.kind.isNotEmpty)
                                      _Presence(text: memberLine(message))
                                    else
                                      _Bubble(
                                        chat: peer,
                                        message: message,
                                        sender: group && message.incoming
                                            ? sender(message)
                                            : null,
                                        joined:
                                            previous != null &&
                                            previous.kind.isEmpty &&
                                            previous.incoming ==
                                                message.incoming &&
                                            previous.from == message.from &&
                                            _day(previous.time) == day,
                                      ),
                                  ],
                                );
                              },
                            ),
                    ),
                    Container(
                      decoration: const BoxDecoration(
                        color: Palette.bg2,
                        border: Border(top: BorderSide(color: Palette.line)),
                      ),
                      padding: EdgeInsets.fromLTRB(
                        Metrics.edge,
                        10,
                        Metrics.edge - 8,
                        inset + 10,
                      ),
                      child: group && !status.gospel
                          ? const Text(
                              'Gospel is off, turn it on to send messages to '
                              'this group.',
                              style: Styles.detail,
                            )
                          : group && !joined
                          ? Row(
                              children: [
                                const Expanded(
                                  child: Text(
                                    'You are not in this group, join it to '
                                    'send messages.',
                                    style: Styles.detail,
                                  ),
                                ),
                                const SizedBox(width: 12),
                                Btn(
                                  label: 'Join',
                                  onTap: () => _membership(true),
                                ),
                                const SizedBox(width: 8),
                              ],
                            )
                          : _Composer(
                              input: _input,
                              busy: _sending,
                              recording: _recording,
                              discard: _discard,
                              onSend: _send,
                              onAttach: _attach,
                              onHold: _hold,
                              onSlide: _slide,
                              onRelease: _release,
                            ),
                    ),
                  ],
                ),
              ),
            ),
          );
        },
      ),
    );
  }
}

class _Member extends StatelessWidget {
  const _Member({required this.member, required this.onTap});

  final ChatMember member;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;
    final named = vigil.names.containsKey(member.id);

    final (state, label, color) = member.typing
        ? (Liveness.online, 'TYPING…', Palette.amber)
        : switch (member.link) {
            'p2p' => (Liveness.online, 'ONLINE · P2P', Palette.good),
            'relay' => (
              Liveness.online,
              'ONLINE · VIA CATHEDRAL',
              Palette.good,
            ),
            'connecting' => (Liveness.pending, 'CONNECTING', Palette.amber),
            _ => (Liveness.off, 'IN THE GROUP', Palette.dim),
          };

    return Panel(
      padding: const EdgeInsets.fromLTRB(16, 12, 12, 12),
      onTap: onTap,
      child: Row(
        children: [
          StateDot(state),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  named || member.nick.isEmpty
                      ? vigil.name(member.id)
                      : member.nick,
                  style: Styles.cardTitle,
                  overflow: TextOverflow.ellipsis,
                ),
                const SizedBox(height: 2),
                Text(
                  named && member.nick.isNotEmpty
                      ? '$label · ${member.nick}'
                      : '$label · PEER ${member.id}',
                  style: Styles.rubric.copyWith(color: color),
                  overflow: TextOverflow.ellipsis,
                ),
              ],
            ),
          ),
          const Glyph(GlyphKind.chat, color: Palette.amber),
        ],
      ),
    );
  }
}

class _Typing extends StatefulWidget {
  const _Typing({this.sender});

  final String? sender;

  @override
  State<_Typing> createState() => _TypingState();
}

class _TypingState extends State<_Typing> with SingleTickerProviderStateMixin {
  late final AnimationController _pulse = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 1200),
  )..repeat();

  @override
  void dispose() {
    _pulse.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final sender = widget.sender;

    return Padding(
      padding: const EdgeInsets.only(top: 10),
      child: Align(
        alignment: Alignment.centerLeft,
        child: Semantics(
          label: sender == null ? 'Typing' : '$sender typing',
          child: CustomPaint(
            painter: const CutPainter(
              fill: Palette.bg2,
              stroke: Palette.line2,
              cut: 8,
            ),
            child: Padding(
              padding: const EdgeInsets.fromLTRB(12, 8, 12, 10),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                mainAxisSize: MainAxisSize.min,
                children: [
                  if (sender != null)
                    Padding(
                      padding: const EdgeInsets.only(bottom: 6),
                      child: Text(
                        sender,
                        style: Styles.rubric.copyWith(color: Palette.amber),
                      ),
                    ),
                  AnimatedBuilder(
                    animation: _pulse,
                    builder: (context, _) => Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        for (var i = 0; i < 3; i++) ...[
                          if (i > 0) const SizedBox(width: 5),
                          Container(width: 6, height: 6, color: _dot(i)),
                        ],
                      ],
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  Color _dot(int i) {
    if ((_pulse.value * 3).floor() == i) {
      return Palette.fg2;
    }

    return Palette.line2;
  }
}

class _Presence extends StatelessWidget {
  const _Presence({required this.text});

  final String text;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(top: 10),
      child: Align(
        alignment: Alignment.centerLeft,
        child: Container(
          padding: const EdgeInsets.fromLTRB(10, 5, 10, 5),
          decoration: BoxDecoration(border: Border.all(color: Palette.line)),
          child: Text(
            text,
            style: Styles.detail.copyWith(fontStyle: FontStyle.italic),
          ),
        ),
      ),
    );
  }
}

Future<void> _save(String chat, ChatMessage message) async {
  unawaited(HapticFeedback.mediumImpact());

  try {
    final saved = await Vigil.instance.saveData(
      chat,
      message.data,
      message.name,
      message.media,
    );

    if (saved) {
      notify('Saved ${message.name}', warn: false);
    }
  } on VigilError catch (e) {
    notify(e.message);
  }
}

class _Photo extends StatelessWidget {
  const _Photo({required this.chat, required this.message});

  final String chat;
  final ChatMessage message;

  @override
  Widget build(BuildContext context) {
    final width = MediaQuery.sizeOf(context).width * 0.6;

    return FutureBuilder<Uint8List?>(
      future: Vigil.instance.chatData(chat, message.data),
      builder: (context, snapshot) {
        final bytes = snapshot.data;

        if (bytes == null) {
          return SizedBox(
            width: width,
            height: width * 0.75,
            child: Center(
              child: Text(
                snapshot.connectionState == ConnectionState.done
                    ? 'Image is gone'
                    : 'Loading…',
                style: Styles.detail,
              ),
            ),
          );
        }

        return GestureDetector(
          onTap: () => push(_PhotoPage(bytes: bytes)),
          onLongPress: () => _save(chat, message),
          child: ConstrainedBox(
            constraints: BoxConstraints(
              maxWidth: width,
              maxHeight: width * 1.4,
            ),
            child: Image.memory(
              bytes,
              fit: BoxFit.contain,
              cacheWidth: (width * MediaQuery.devicePixelRatioOf(context))
                  .round(),
              gaplessPlayback: true,
            ),
          ),
        );
      },
    );
  }
}

class _File extends StatelessWidget {
  const _File({required this.chat, required this.message});

  final String chat;
  final ChatMessage message;

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: () => Vigil.instance
          .openData(chat, message.data, message.name)
          .catchError((Object e) => notify('$e')),
      onLongPress: () => _save(chat, message),
      child: ConstrainedBox(
        constraints: BoxConstraints(
          maxWidth: MediaQuery.sizeOf(context).width * 0.6,
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Glyph(GlyphKind.paste, size: 22, color: Palette.amber),
            const SizedBox(width: 10),
            Flexible(
              child: Text(
                message.name,
                style: Styles.body.copyWith(color: Palette.fg),
                overflow: TextOverflow.ellipsis,
                maxLines: 2,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _PhotoPage extends StatelessWidget {
  const _PhotoPage({required this.bytes});

  final Uint8List bytes;

  @override
  Widget build(BuildContext context) {
    return ColoredBox(
      color: Palette.bg,
      child: SafeArea(
        child: Stack(
          children: [
            Positioned.fill(
              child: InteractiveViewer(
                maxScale: 6,
                child: Center(child: Image.memory(bytes)),
              ),
            ),
            Positioned(
              top: 4,
              left: Metrics.edge - 12,
              child: GlyphBtn(
                GlyphKind.close,
                label: 'Close',
                onTap: () => Navigator.of(context).maybePop(),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _Bubble extends StatelessWidget {
  const _Bubble({
    required this.chat,
    required this.message,
    required this.joined,
    this.sender,
  });

  final String chat;
  final ChatMessage message;
  final bool joined;
  final String? sender;

  @override
  Widget build(BuildContext context) {
    final incoming = message.incoming;
    final width = MediaQuery.sizeOf(context).width;
    final header = sender != null && !joined;

    final acked = message.acked;
    final pending = message.pending;

    final tick = switch (message.state) {
      'delivered' => (acked == null ? '✓✓' : '✓✓ $acked', Palette.amber),
      'sent' when acked != null => ('✓ $acked · $pending pending', Palette.fg2),
      'sent' => ('✓', Palette.fg2),
      'failed' => ('✕ not sent', Palette.amber2),
      _ => ('◷', Palette.dim),
    };

    return Padding(
      padding: EdgeInsets.only(top: joined ? 4 : 10),
      child: Align(
        alignment: incoming ? Alignment.centerLeft : Alignment.centerRight,
        child: ConstrainedBox(
          constraints: BoxConstraints(maxWidth: width * 0.78),
          child: CustomPaint(
            painter: CutPainter(
              fill: incoming ? Palette.bg2 : Palette.amberWash,
              stroke: incoming ? Palette.line2 : Palette.amberDim,
              cut: 8,
            ),
            child: Padding(
              padding: const EdgeInsets.fromLTRB(12, 8, 12, 7),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.end,
                mainAxisSize: MainAxisSize.min,
                children: [
                  Stack(
                    children: [
                      Align(
                        alignment: Alignment.centerLeft,
                        widthFactor: 1,
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            if (header)
                              Padding(
                                padding: const EdgeInsets.only(bottom: 3),
                                child: Row(
                                  mainAxisSize: MainAxisSize.min,
                                  children: [
                                    Flexible(
                                      child: Text(
                                        sender!,
                                        overflow: TextOverflow.ellipsis,
                                        style: Styles.rubric.copyWith(
                                          color: Palette.amber,
                                        ),
                                      ),
                                    ),
                                    const SizedBox(width: 12),
                                    ExcludeSemantics(
                                      child: Opacity(
                                        opacity: 0,
                                        child: Text(
                                          message.from,
                                          style: Styles.rubric,
                                        ),
                                      ),
                                    ),
                                  ],
                                ),
                              ),
                            if (message.data.isEmpty)
                              SelectableText(
                                message.text,
                                style: Styles.body.copyWith(color: Palette.fg),
                              )
                            else if (message.media == 'image')
                              _Photo(chat: chat, message: message)
                            else if (message.media == 'voice')
                              _VoiceNote(
                                key: ValueKey('$chat/${message.data}'),
                                chat: chat,
                                message: message,
                              )
                            else
                              _File(chat: chat, message: message),
                          ],
                        ),
                      ),
                      if (header)
                        Positioned(
                          top: 0,
                          right: 0,
                          child: Semantics(
                            label: 'KEK ID ${message.from}',
                            child: Text(message.from, style: Styles.rubric),
                          ),
                        ),
                    ],
                  ),
                  const SizedBox(height: 3),
                  Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Text(
                        _clock(message.time),
                        style: Styles.detail.copyWith(fontSize: 10.5),
                      ),
                      if (!incoming) ...[
                        const SizedBox(width: 6),
                        Semantics(
                          label: acked == null
                              ? message.state
                              : '$acked acked, $pending pending',
                          child: Text(
                            tick.$1,
                            style: Styles.detail.copyWith(
                              fontSize: 10.5,
                              color: tick.$2,
                            ),
                          ),
                        ),
                      ],
                    ],
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _Composer extends StatelessWidget {
  const _Composer({
    required this.input,
    required this.busy,
    required this.recording,
    required this.discard,
    required this.onSend,
    required this.onAttach,
    required this.onHold,
    required this.onSlide,
    required this.onRelease,
  });

  final TextEditingController input;
  final bool busy;
  final DateTime? recording;
  final bool discard;
  final VoidCallback onSend;
  final VoidCallback onAttach;
  final VoidCallback onHold;
  final ValueChanged<Offset> onSlide;
  final Future<void> Function({bool cancel}) onRelease;

  Widget _recorder(DateTime started) {
    final elapsed = DateTime.now().difference(started).inSeconds;

    return CustomPaint(
      painter: CutPainter(
        fill: Palette.bg,
        stroke: discard ? Palette.amber2 : Palette.amberDim,
        cut: 8,
      ),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
        child: Row(
          children: [
            _Pulse(color: discard ? Palette.dim : Palette.amber),
            const SizedBox(width: 10),
            Text(
              _length(elapsed),
              style: Styles.body.copyWith(color: Palette.fg),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Text(
                discard ? 'Release to discard' : '‹ Slide to discard',
                overflow: TextOverflow.ellipsis,
                textAlign: TextAlign.right,
                style: Styles.detail.copyWith(
                  color: discard ? Palette.amber2 : Palette.dim,
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _mic() {
    final Color color;

    if (busy) {
      color = Palette.dim;
    } else if (recording != null) {
      color = Palette.amber;
    } else {
      color = Palette.fg2;
    }

    return Semantics(
      button: true,
      label: 'Hold to record a voice message',
      child: Listener(
        behavior: HitTestBehavior.opaque,
        onPointerDown: busy ? null : (_) => onHold(),
        onPointerMove: (e) => onSlide(e.localPosition - const Offset(22, 22)),
        onPointerUp: (_) => onRelease(),
        onPointerCancel: (_) => onRelease(cancel: true),
        child: SizedBox(
          width: 44,
          height: 44,
          child: Center(child: Glyph(GlyphKind.mic, color: color)),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<TextEditingValue>(
      valueListenable: input,
      builder: (context, value, _) {
        final used = utf8.encode(value.text).length;
        final ready = value.text.trim().isNotEmpty && !busy;

        return Row(
          crossAxisAlignment: CrossAxisAlignment.end,
          children: [
            GlyphBtn(
              GlyphKind.plus,
              label: 'Send an image or a file',
              onTap: busy ? null : onAttach,
            ),
            const SizedBox(width: 4),
            Expanded(
              child: recording != null
                  ? _recorder(recording!)
                  : Column(
                      crossAxisAlignment: CrossAxisAlignment.end,
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        if (used > _messageMax - 112)
                          Padding(
                            padding: const EdgeInsets.only(bottom: 4),
                            child: Text(
                              '$used/$_messageMax',
                              style: Styles.detail.copyWith(
                                fontSize: 10.5,
                                color: used >= _messageMax
                                    ? Palette.amber
                                    : Palette.dim,
                              ),
                            ),
                          ),
                        CustomPaint(
                          painter: const CutPainter(
                            fill: Palette.bg,
                            stroke: Palette.line2,
                            cut: 8,
                          ),
                          child: Padding(
                            padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
                            child: TextField(
                              controller: input,
                              minLines: 1,
                              maxLines: 5,
                              keyboardType: TextInputType.multiline,
                              textCapitalization: TextCapitalization.sentences,
                              inputFormatters: const [_Utf8Limit(_messageMax)],
                              style: Styles.body.copyWith(color: Palette.fg),
                              cursorColor: Palette.amber,
                              decoration: InputDecoration.collapsed(
                                hintText: 'Message',
                                hintStyle: Styles.body.copyWith(
                                  color: Palette.dim,
                                ),
                              ),
                            ),
                          ),
                        ),
                      ],
                    ),
            ),
            _mic(),
            GlyphBtn(
              GlyphKind.send,
              label: 'Send',
              onTap: ready && recording == null ? onSend : null,
            ),
          ],
        );
      },
    );
  }
}

String _length(int seconds) =>
    '${seconds ~/ 60}:${(seconds % 60).toString().padLeft(2, '0')}';

class _Pulse extends StatefulWidget {
  const _Pulse({required this.color});

  final Color color;

  @override
  State<_Pulse> createState() => _PulseState();
}

class _PulseState extends State<_Pulse> with SingleTickerProviderStateMixin {
  late final _beat = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 900),
  )..repeat(reverse: true);

  @override
  void dispose() {
    _beat.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return FadeTransition(
      opacity: Tween(begin: 0.3, end: 1.0).animate(_beat),
      child: Container(width: 10, height: 10, color: widget.color),
    );
  }
}

class _Voice {
  const _Voice(this.seconds, this.bars);

  final int seconds;
  final List<double> bars;
}

const _adtsRates = [
  96000,
  88200,
  64000,
  48000,
  44100,
  32000,
  24000,
  22050,
  16000,
  12000,
  11025,
  8000,
  7350,
];

List<double> _voiceShape(String seed) {
  final bars = <double>[];
  var x = seed.hashCode & 0x7fffffff;

  for (var i = 0; i < _voiceBars; i++) {
    x = (x * 1103515245 + 12345) & 0x7fffffff;
    bars.add(0.25 + (x % 1000) / 1333);
  }

  return bars;
}

_Voice _voice(Uint8List bytes, String seed) {
  final sizes = <int>[];
  var rate = 0;
  var off = 0;

  while (off + 7 <= bytes.length) {
    if (bytes[off] != 0xff || (bytes[off + 1] & 0xf6) != 0xf0) {
      break;
    }

    final index = (bytes[off + 2] >> 2) & 0x0f;
    if (index >= _adtsRates.length) {
      break;
    }

    final len =
        ((bytes[off + 3] & 0x03) << 11) |
        (bytes[off + 4] << 3) |
        (bytes[off + 5] >> 5);
    if (len <= 7) {
      break;
    }

    rate = _adtsRates[index];
    sizes.add(len);
    off += len;
  }

  if (sizes.isEmpty) {
    return _Voice(0, _voiceShape(seed));
  }

  final seconds = (sizes.length * 1024 / rate).round();
  final bars = <double>[];

  for (var i = 0; i < _voiceBars; i++) {
    final from = i * sizes.length ~/ _voiceBars;
    final to = (i + 1) * sizes.length ~/ _voiceBars;
    var sum = 0;

    for (var j = from; j < to; j++) {
      sum += sizes[j];
    }

    bars.add(to > from ? sum / (to - from) : 0);
  }

  final low = bars.reduce((a, b) => a < b ? a : b);
  final high = bars.reduce((a, b) => a > b ? a : b);

  if (high - low < high * 0.08) {
    return _Voice(seconds, _voiceShape(seed));
  }

  return _Voice(seconds, [
    for (final b in bars) 0.15 + 0.85 * (b - low) / (high - low),
  ]);
}

class _VoiceNote extends StatefulWidget {
  const _VoiceNote({super.key, required this.chat, required this.message});

  final String chat;
  final ChatMessage message;

  @override
  State<_VoiceNote> createState() => _VoiceNoteState();
}

class _VoiceNoteState extends State<_VoiceNote>
    with SingleTickerProviderStateMixin {
  late final _progress = AnimationController(vsync: this);
  late final Future<_Voice> _shape = _load();

  String get _key => '${widget.chat}/${widget.message.data}';

  Future<_Voice> _load() async {
    final bytes = await Vigil.instance.chatData(
      widget.chat,
      widget.message.data,
    );

    if (bytes == null) {
      return _Voice(0, _voiceShape(widget.message.data));
    }

    return _voice(bytes, widget.message.data);
  }

  @override
  void initState() {
    super.initState();
    _playing.addListener(_changed);
  }

  @override
  void dispose() {
    _playing.removeListener(_changed);
    _progress.dispose();
    super.dispose();
  }

  void _changed() {
    if (_playing.value != _key) {
      _progress.reset();
    }
  }

  Future<void> _toggle() async {
    final vigil = Vigil.instance;

    if (_playing.value == _key) {
      _playing.value = null;
      await vigil.stopVoice().catchError((Object _) {});
      return;
    }

    final voice = await _shape;

    _playing.value = _key;
    _progress.duration = Duration(seconds: voice.seconds.clamp(1, 3600));
    unawaited(_progress.forward(from: 0));

    try {
      await vigil.playVoice(widget.chat, widget.message.data);
    } on VigilError catch (e) {
      notify(e.message);
    }

    if (_playing.value == _key) {
      _playing.value = null;
    }
  }

  @override
  Widget build(BuildContext context) {
    final width = MediaQuery.sizeOf(context).width * 0.42;

    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: _toggle,
      onLongPress: () => _save(widget.chat, widget.message),
      child: FutureBuilder<_Voice>(
        future: _shape,
        builder: (context, snapshot) {
          final voice = snapshot.data;

          return ValueListenableBuilder<String?>(
            valueListenable: _playing,
            builder: (context, playing, _) {
              final on = playing == _key;

              return Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Semantics(
                    button: true,
                    label: on ? 'Stop voice message' : 'Play voice message',
                    child: Container(
                      width: 34,
                      height: 34,
                      decoration: BoxDecoration(
                        border: Border.all(color: Palette.amberDim),
                      ),
                      child: Center(
                        child: Glyph(
                          on ? GlyphKind.stop : GlyphKind.play,
                          size: 16,
                          color: Palette.amber,
                        ),
                      ),
                    ),
                  ),
                  const SizedBox(width: 10),
                  AnimatedBuilder(
                    animation: _progress,
                    builder: (context, _) => CustomPaint(
                      size: Size(width, 28),
                      painter: _WavePainter(
                        bars: voice?.bars ?? _voiceShape(widget.message.data),
                        played: _progress.value,
                      ),
                    ),
                  ),
                  const SizedBox(width: 10),
                  Text(
                    voice == null ? '-:--' : _length(voice.seconds),
                    style: Styles.detail.copyWith(fontSize: 10.5),
                  ),
                ],
              );
            },
          );
        },
      ),
    );
  }
}

class _WavePainter extends CustomPainter {
  const _WavePainter({required this.bars, required this.played});

  final List<double> bars;
  final double played;

  @override
  void paint(Canvas canvas, Size size) {
    final step = size.width / bars.length;
    final lit = Paint()..color = Palette.amber;
    final dim = Paint()..color = Palette.fg2.withValues(alpha: 0.45);

    for (var i = 0; i < bars.length; i++) {
      final h = size.height * bars[i];
      final rect = Rect.fromLTWH(
        i * step + step * 0.2,
        (size.height - h) / 2,
        step * 0.6,
        h,
      );

      canvas.drawRect(rect, (i + 0.5) / bars.length <= played ? lit : dim);
    }
  }

  @override
  bool shouldRepaint(covariant _WavePainter oldDelegate) =>
      oldDelegate.played != played || oldDelegate.bars != bars;
}

class _Utf8Limit extends TextInputFormatter {
  const _Utf8Limit(this.max);

  final int max;

  @override
  TextEditingValue formatEditUpdate(
    TextEditingValue oldValue,
    TextEditingValue newValue,
  ) => utf8.encode(newValue.text).length <= max ? newValue : oldValue;
}

Future<void> joinGroup() async {
  final context = navigatorKey.currentContext;
  if (context == null) {
    return;
  }

  final vigil = Vigil.instance;
  final current = vigil.status.chats.group;
  final id = TextEditingController();
  final name = TextEditingController();

  final join = await sheet<bool>(
    context,
    (context) => Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        sheetTitle(current == null ? 'Join a group' : 'Swap group'),
        const SizedBox(height: 8),
        const Text(
          'A group is a 4 digit hex id, everyone in your flock who joins '
          'the same id is in it. You can be in one group at a time.',
          style: Styles.detail,
        ),
        const SizedBox(height: 16),
        Text('GROUP ID', style: Styles.key),
        const SizedBox(height: 6),
        Pre(
          child: TextField(
            controller: id,
            autofocus: true,
            maxLength: 4,
            style: Styles.pre,
            cursorColor: Palette.amber,
            inputFormatters: [
              FilteringTextInputFormatter.allow(RegExp('[0-9A-Fa-f]')),
            ],
            decoration: InputDecoration.collapsed(
              hintText: '1a2b',
              hintStyle: Styles.pre.copyWith(color: Palette.dim),
            ).copyWith(counterText: ''),
          ),
        ),
        const SizedBox(height: 14),
        Text('NAME', style: Styles.key),
        const SizedBox(height: 6),
        Pre(
          child: TextField(
            controller: name,
            maxLength: 32,
            textCapitalization: TextCapitalization.words,
            style: Styles.pre,
            cursorColor: Palette.amber,
            decoration: InputDecoration.collapsed(
              hintText: 'Only on this device',
              hintStyle: Styles.pre.copyWith(color: Palette.dim),
            ).copyWith(counterText: ''),
          ),
        ),
        const SizedBox(height: 20),
        Btn(
          label: 'Join',
          expand: true,
          onTap: () => Navigator.of(context).pop(true),
        ),
      ],
    ),
  );

  final raw = id.text;
  final hex = raw.toLowerCase().padLeft(4, '0');
  final label = name.text.trim();
  id.dispose();
  name.dispose();

  if (join != true || raw.isEmpty) {
    return;
  }

  if (hex == '0000') {
    notify('Group 0000 is where chats signal, pick another one.');
    return;
  }

  final chat = 'g$hex';

  if (current != null && current != chat) {
    if (!await confirm(
      title: 'Switch groups',
      text:
          'You can be in one group at a time, joining ${vigil.name(chat)} '
          'leaves ${vigil.name(current)}.',
      action: 'Join',
    )) {
      return;
    }
  }

  try {
    await vigil.joinGroup(hex);
    if (label.isNotEmpty) {
      await vigil.rename(chat, label);
    }

    await push(ChatPage(peer: chat));
  } on VigilError catch (e) {
    notify(e.message);
  }
}

Future<void> chatSettings() async {
  final context = navigatorKey.currentContext;
  if (context == null) {
    return;
  }

  final vigil = Vigil.instance;
  ChatSettings settings;

  try {
    settings = await vigil.chatSettings();
  } on VigilError catch (e) {
    notify(e.message);
    return;
  }

  final nick = TextEditingController(text: settings.nick);
  var preview = settings.preview;

  if (!context.mounted) {
    return;
  }

  final saved = await sheet<bool>(
    context,
    (context) => StatefulBuilder(
      builder: (context, setState) => Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          sheetTitle('Chat settings'),
          const SizedBox(height: 16),
          Text('YOUR NAME', style: Styles.key),
          const SizedBox(height: 6),
          Pre(
            child: TextField(
              controller: nick,
              maxLength: 16,
              style: Styles.pre,
              cursorColor: Palette.amber,
              inputFormatters: [
                FilteringTextInputFormatter.allow(RegExp('[0-9A-Za-z]')),
              ],
              decoration: InputDecoration.collapsed(
                hintText: 'Sent to your peers, gospel shows it',
                hintStyle: Styles.pre.copyWith(color: Palette.dim),
              ).copyWith(counterText: ''),
            ),
          ),
          const SizedBox(height: 18),
          Row(
            children: [
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text('Show message text', style: Styles.cardTitle),
                    const SizedBox(height: 3),
                    const Text(
                      'In notifications, otherwise they only say there is '
                      'a new message.',
                      style: Styles.detail,
                    ),
                  ],
                ),
              ),
              Toggle(
                on: preview,
                label: 'Show message text',
                onTap: () => setState(() => preview = !preview),
              ),
            ],
          ),
          const SizedBox(height: 20),
          Btn(
            label: 'Save',
            expand: true,
            onTap: () => Navigator.of(context).pop(true),
          ),
        ],
      ),
    ),
  );

  final name = nick.text;
  nick.dispose();

  if (saved != true) {
    return;
  }

  try {
    await vigil.chatSettings(nick: name, preview: preview);
  } on VigilError catch (e) {
    notify(e.message);
  }
}
