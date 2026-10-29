import 'dart:async';
import 'dart:convert';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:image_picker/image_picker.dart';
import 'package:url_launcher/url_launcher.dart';

import '../app.dart';
import '../links.dart';
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
const _voiceRates = [1.0, 1.5, 2.0];
var _voiceRate = 1.0;

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

  return chosen(message.nick);
}

bool senderPicked(ChatMessage message) {
  final vigil = Vigil.instance;

  if (vigil.names.containsKey(message.from)) {
    return false;
  }

  if (message.nick.isEmpty) {
    return vigil.picked(message.from);
  }

  return true;
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
  final chats = await vigil.chats().catchError((Object _) => <ChatSummary>[]);
  final peers = <String>{
    ...?status.call?.peers,
    ...?status.chat?.peers.where((p) => p.present).map((p) => p.id),
    ...vigil.names.keys.where((p) => !isGroup(p)),
    ...chats.map((c) => c.peer).where((p) => !isGroup(p)),
  }.where((p) => p != own).toList()..sort();

  if (!context.mounted) {
    return;
  }

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
        Padding(
          padding: const EdgeInsets.only(bottom: 8),
          child: Panel(
            padding: const EdgeInsets.fromLTRB(16, 12, 12, 12),
            onTap: () => Navigator.of(context).pop('#'),
            child: Row(
              children: [
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text('Chat by KEK ID', style: Styles.cardTitle),
                      const Text(
                        'Anyone in your flock, messages wait until they are '
                        'online',
                        style: Styles.detail,
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
            'Nobody is around right now. Use a KEK ID to leave someone a '
            'message.',
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
                          style: peerStyle(Styles.cardTitle, vigil.picked(id)),
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
  } else if (peer == '#') {
    await _chatById();
  } else if (peer != null) {
    await push(ChatPage(peer: peer));
  }
}

Future<void> _chatById() async {
  final context = navigatorKey.currentContext;
  if (context == null) {
    return;
  }

  final raw = await sheet<String>(context, (context) => const _KekPrompt());

  if (raw == null || raw.isEmpty) {
    return;
  }

  final hex = raw.toLowerCase().padLeft(2, '0');

  if (hex == '00' || hex == 'ff') {
    notify('KEK IDs go from 01 to fe.');
    return;
  }

  if (hex == ownPeer()) {
    notify('That is you.');
    return;
  }

  await push(ChatPage(peer: hex));
}

class _KekPrompt extends StatefulWidget {
  const _KekPrompt();

  @override
  State<_KekPrompt> createState() => _KekPromptState();
}

class _KekPromptState extends State<_KekPrompt> {
  final _id = TextEditingController();

  @override
  void dispose() {
    _id.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        sheetTitle('Chat by KEK ID'),
        const SizedBox(height: 8),
        const Text(
          'The 2 digit hex id of someone in your flock. They get your '
          'messages once they are online.',
          style: Styles.detail,
        ),
        const SizedBox(height: 16),
        Text('KEK ID', style: Styles.key),
        const SizedBox(height: 6),
        Pre(
          child: TextField(
            controller: _id,
            autofocus: true,
            maxLength: 2,
            style: Styles.pre,
            cursorColor: Palette.amber,
            inputFormatters: [
              FilteringTextInputFormatter.allow(RegExp('[0-9A-Fa-f]')),
            ],
            decoration: InputDecoration.collapsed(
              hintText: '0a',
              hintStyle: Styles.pre.copyWith(color: Palette.dim),
            ).copyWith(counterText: ''),
          ),
        ),
        const SizedBox(height: 20),
        Btn(
          label: 'Open chat',
          expand: true,
          onTap: () => Navigator.of(context).pop(_id.text),
        ),
      ],
    );
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
                  style: peerStyle(Styles.cardTitle, vigil.picked(chat.peer)),
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
  bool _pastable = false;
  bool _discard = false;
  bool _starting = false;
  bool _locked = false;
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
            chosen(m.nick),
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
    unawaited(_clipboard());
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

    unawaited(vigil.typing(peer, false).catchError((Object _) {}));
    unawaited(vigil.openChat(peer, false).catchError((Object _) {}));

    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.paused) {
      _held = false;
      _locked = false;
      unawaited(_release(cancel: true));
    }

    if (state == AppLifecycleState.resumed) {
      unawaited(Vigil.instance.openChat(peer, true).catchError((Object _) {}));
      unawaited(_clipboard());
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

  Future<void> _clipboard() async {
    final pastable = await Vigil.instance.clipboardImage().catchError(
      (Object _) => false,
    );

    if (mounted && pastable != _pastable) {
      setState(() => _pastable = pastable);
    }
  }

  Future<void> _paste() async {
    if (_sending) {
      return;
    }

    final Uint8List image;

    try {
      image = await Vigil.instance.clipboardImageData();
    } on VigilError catch (e) {
      notify(e.message);
      return;
    }

    if (!mounted) {
      return;
    }

    final height = MediaQuery.sizeOf(context).height * 0.45;
    final send = await confirm(
      title: 'Send this image?',
      text: 'This is the image on your clipboard.',
      action: 'Send',
      detail: Center(
        child: ConstrainedBox(
          constraints: BoxConstraints(maxHeight: height),
          child: Image.memory(image, fit: BoxFit.contain),
        ),
      ),
    );

    if (send) {
      await _sendWith(() => Vigil.instance.sendImageData(peer, image));
    }

    unawaited(_clipboard());
  }

  Future<void> _hold() async {
    if (_sending) {
      return;
    }

    if (_held || _recording != null) {
      return;
    }

    _held = true;

    setState(() {
      _discard = false;
      _locked = false;
      _starting = true;
    });

    try {
      await Vigil.instance.recordVoice(peer);
    } on VigilError catch (e) {
      _held = false;
      if (mounted) {
        setState(() => _starting = false);
      }
      notify(e.message);
      return;
    }

    if (!mounted) {
      await Vigil.instance.cancelVoice().catchError((Object _) {});
      return;
    }

    if (!_held && !_locked) {
      setState(() => _starting = false);
      await Vigil.instance.cancelVoice().catchError((Object _) {});
      notify('Hold to record a voice message', warn: false);
      return;
    }

    unawaited(HapticFeedback.mediumImpact());

    setState(() {
      _starting = false;
      _recording = DateTime.now();
    });
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
    if (_locked) {
      return;
    }

    if (_recording == null && !_starting) {
      return;
    }

    final discard = offset.dx < -72;

    if (!discard && offset.dy < -60) {
      unawaited(HapticFeedback.mediumImpact());
      setState(() {
        _locked = true;
        _discard = false;
      });
      return;
    }

    if (discard != _discard) {
      unawaited(HapticFeedback.selectionClick());
      setState(() => _discard = discard);
    }
  }

  void _lift({bool cancel = false}) {
    _held = false;

    if (_locked) {
      return;
    }

    unawaited(_release(cancel: cancel));
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
      _locked = false;
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
                                    style: peerStyle(
                                      Styles.barTitle,
                                      vigil.picked(peer),
                                    ),
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
                              starting: _starting,
                              locked: _locked,
                              discard: _discard,
                              onSend: _send,
                              onAttach: _attach,
                              onPaste: _pastable ? _paste : null,
                              onMenu: _clipboard,
                              onHold: _hold,
                              onSlide: _slide,
                              onLift: _lift,
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
                      : chosen(member.nick),
                  style: peerStyle(
                    Styles.cardTitle,
                    !named && member.nick.isNotEmpty,
                  ),
                  overflow: TextOverflow.ellipsis,
                ),
                const SizedBox(height: 2),
                Text(
                  named && member.nick.isNotEmpty
                      ? '$label · ${chosen(member.nick)}'
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
                                        style: peerStyle(
                                          Styles.rubric.copyWith(
                                            color: Palette.amber,
                                          ),
                                          senderPicked(message),
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
                              _Text(message.text)
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
    required this.starting,
    required this.locked,
    required this.discard,
    required this.onSend,
    required this.onAttach,
    required this.onPaste,
    required this.onMenu,
    required this.onHold,
    required this.onSlide,
    required this.onLift,
    required this.onRelease,
  });

  final TextEditingController input;
  final bool busy;
  final DateTime? recording;
  final bool starting;
  final bool locked;
  final bool discard;
  final VoidCallback onSend;
  final VoidCallback onAttach;
  final VoidCallback? onPaste;
  final VoidCallback onMenu;
  final VoidCallback onHold;
  final ValueChanged<Offset> onSlide;
  final void Function({bool cancel}) onLift;
  final Future<void> Function({bool cancel}) onRelease;

  String _hint() {
    if (recording == null) {
      return 'Starting…';
    }

    if (discard) {
      return 'Release to discard';
    }

    if (locked) {
      return 'Locked';
    }

    return '‹ Discard · ↑ Lock';
  }

  Widget _recorder() {
    final started = recording;
    var elapsed = 0;

    if (started != null) {
      elapsed = DateTime.now().difference(started).inSeconds;
    }

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
            if (started == null || discard)
              Container(width: 10, height: 10, color: Palette.dim)
            else
              const _Pulse(color: Palette.amber),
            const SizedBox(width: 10),
            Text(
              _length(elapsed),
              style: Styles.body.copyWith(color: Palette.fg),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Text(
                _hint(),
                overflow: TextOverflow.ellipsis,
                textAlign: TextAlign.right,
                style: Styles.detail.copyWith(
                  color: discard ? Palette.amber2 : Palette.dim,
                ),
              ),
            ),
            if (locked)
              GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTap: () => onRelease(cancel: true),
                child: Semantics(
                  button: true,
                  label: 'Discard the voice message',
                  child: const Padding(
                    padding: EdgeInsets.only(left: 12),
                    child: Glyph(GlyphKind.close, size: 18),
                  ),
                ),
              ),
          ],
        ),
      ),
    );
  }

  Widget _menu(BuildContext context, EditableTextState state) {
    onMenu();

    final items = state.contextMenuButtonItems;
    final text = items.any((i) => i.type == ContextMenuButtonType.paste);

    return AdaptiveTextSelectionToolbar.buttonItems(
      anchors: state.contextMenuAnchors,
      buttonItems: [
        ...items,
        if (onPaste != null && !text)
          ContextMenuButtonItem(
            type: ContextMenuButtonType.paste,
            onPressed: () {
              ContextMenuController.removeAny();
              onPaste!();
            },
          ),
      ],
    );
  }

  VoidCallback? _onSend(bool ready) {
    if (locked && recording != null) {
      return onRelease;
    }

    if (ready && recording == null && !starting) {
      return onSend;
    }

    return null;
  }

  Widget _mic() {
    final Color color;

    if (busy) {
      color = Palette.dim;
    } else if (recording != null || starting) {
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
        onPointerUp: (_) => onLift(),
        onPointerCancel: (_) => onLift(cancel: true),
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
              child: recording != null || starting
                  ? _recorder()
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
                              contextMenuBuilder: _menu,
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
            GlyphBtn(GlyphKind.send, label: 'Send', onTap: _onSend(ready)),
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

class _VoiceNoteState extends State<_VoiceNote> {
  late final Future<_Voice> _shape = _load();
  Timer? _poll;
  int _at = 0;
  int _total = 0;

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

    if (_playing.value == _key) {
      _follow();
    }
  }

  @override
  void dispose() {
    _playing.removeListener(_changed);
    _poll?.cancel();
    super.dispose();
  }

  void _changed() {
    if (_playing.value == _key) {
      _follow();
      return;
    }

    _poll?.cancel();
    _poll = null;

    if (mounted) {
      setState(() => _at = 0);
    }
  }

  void _follow() {
    _poll ??= Timer.periodic(const Duration(milliseconds: 100), (_) async {
      final position = await Vigil.instance.voicePosition().catchError(
        (Object _) => null,
      );

      if (mounted && position != null && _playing.value == _key) {
        setState(() {
          _at = position.$1;
          _total = position.$2;
        });
      }
    });
  }

  Future<void> _play({int at = 0}) async {
    _playing.value = _key;

    try {
      await Vigil.instance.playVoice(
        widget.chat,
        widget.message.data,
        at: at,
        rate: _voiceRate,
      );
    } on VigilError catch (e) {
      notify(e.message);
    }

    if (_playing.value == _key) {
      _playing.value = null;
    }
  }

  Future<void> _toggle() async {
    if (_playing.value == _key) {
      _playing.value = null;
      await Vigil.instance.stopVoice().catchError((Object _) {});
      return;
    }

    await _play();
  }

  Future<void> _seek(double fraction, {bool start = true}) async {
    final voice = await _shape;
    var total = _total;

    if (total == 0) {
      total = voice.seconds * 1000;
    }

    final at = (fraction.clamp(0.0, 1.0) * total).round();

    if (_playing.value != _key) {
      if (start) {
        await _play(at: at);
      }
      return;
    }

    setState(() => _at = at);
    await Vigil.instance.seekVoice(at).catchError((Object _) {});
  }

  Future<void> _speed() async {
    final next = _voiceRates.indexOf(_voiceRate) + 1;

    setState(() => _voiceRate = _voiceRates[next % _voiceRates.length]);

    if (_playing.value == _key) {
      await Vigil.instance.speedVoice(_voiceRate).catchError((Object _) {});
    }
  }

  @override
  Widget build(BuildContext context) {
    final width = MediaQuery.sizeOf(context).width * 0.38;

    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onLongPress: () => _save(widget.chat, widget.message),
      child: FutureBuilder<_Voice>(
        future: _shape,
        builder: (context, snapshot) {
          final voice = snapshot.data;

          return ValueListenableBuilder<String?>(
            valueListenable: _playing,
            builder: (context, playing, _) {
              final on = playing == _key;
              var played = 0.0;
              var time = '-:--';

              if (on && _total > 0) {
                played = _at / _total;
              }

              if (on) {
                time = _length(_at ~/ 1000);
              } else if (voice != null) {
                time = _length(voice.seconds);
              }

              return Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  GestureDetector(
                    behavior: HitTestBehavior.opaque,
                    onTap: _toggle,
                    child: Semantics(
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
                  ),
                  const SizedBox(width: 10),
                  GestureDetector(
                    behavior: HitTestBehavior.opaque,
                    onTapUp: (d) => _seek(d.localPosition.dx / width),
                    onHorizontalDragUpdate: (d) =>
                        _seek(d.localPosition.dx / width, start: false),
                    child: CustomPaint(
                      size: Size(width, 28),
                      painter: _WavePainter(
                        bars: voice?.bars ?? _voiceShape(widget.message.data),
                        played: played,
                      ),
                    ),
                  ),
                  const SizedBox(width: 10),
                  Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Text(time, style: Styles.detail.copyWith(fontSize: 10.5)),
                      GestureDetector(
                        behavior: HitTestBehavior.opaque,
                        onTap: _speed,
                        child: Semantics(
                          button: true,
                          label: 'Playback speed',
                          child: Padding(
                            padding: const EdgeInsets.only(top: 4),
                            child: Text(
                              _rate(_voiceRate),
                              style: Styles.detail.copyWith(
                                fontSize: 10.5,
                                color: Palette.amber,
                              ),
                            ),
                          ),
                        ),
                      ),
                    ],
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

String _rate(double rate) {
  if (rate == rate.roundToDouble()) {
    return '${rate.toInt()}×';
  }

  return '$rate×';
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

class _Text extends StatefulWidget {
  const _Text(this.text);

  final String text;

  @override
  State<_Text> createState() => _TextState();
}

class _TextState extends State<_Text> {
  final _taps = <TapGestureRecognizer>[];

  @override
  void dispose() {
    _release();
    super.dispose();
  }

  void _release() {
    for (final tap in _taps) {
      tap.dispose();
    }

    _taps.clear();
  }

  @override
  Widget build(BuildContext context) {
    final text = widget.text;
    final style = Styles.body.copyWith(color: Palette.fg);
    final found = links(text);

    if (found.isEmpty) {
      return SelectableText(text, style: style);
    }

    _release();

    final spans = <TextSpan>[];
    var at = 0;

    for (final link in found) {
      final raw = text.substring(link.start, link.end);
      final tap = TapGestureRecognizer()..onTap = () => _visit(raw);

      _taps.add(tap);
      spans.add(TextSpan(text: text.substring(at, link.start)));
      spans.add(
        TextSpan(
          text: raw,
          recognizer: tap,
          style: const TextStyle(
            color: Palette.amber,
            decoration: TextDecoration.underline,
            decorationColor: Palette.amber,
          ),
        ),
      );
      at = link.end;
    }

    spans.add(TextSpan(text: text.substring(at)));

    return SelectableText.rich(TextSpan(style: style, children: spans));
  }
}

Future<void> _visit(String raw) async {
  final link = canonical(raw);

  if (link == null) {
    return;
  }

  final warnings = [
    if (link.user)
      'This link is suspicious, it contains "${link.uri.userInfo}" to '
          'obfuscate the real address.',
    if (link.foreign)
      'This link is using special characters to imitate another website '
          '("${link.shown}")',
    if (link.address)
      'This link does not contain a FQDN, this is usually not normal.',
    if (link.plain)
      'This link lacks TLS, anything sent between you and this site can be '
          'read by anyone.',
  ];

  final go = await confirm(
    title: warnings.isEmpty ? 'Visit link?' : 'Careful with this link',
    risky: warnings.isNotEmpty,
    text: 'Check the link before you tap Visit',
    action: warnings.isEmpty ? 'Visit' : 'Visit anyway',
    detail: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(link.host, style: Styles.cardTitle.copyWith(color: Palette.amber)),
        const SizedBox(height: 8),
        SelectableText(link.uri.toString(), style: Styles.pre),
        for (final warning in warnings) ...[
          const SizedBox(height: 10),
          Text(warning, style: Styles.detail.copyWith(color: Palette.amber2)),
        ],
      ],
    ),
  );

  if (!go) {
    return;
  }

  try {
    await launchUrl(link.uri, mode: LaunchMode.externalApplication);
  } on PlatformException catch (e) {
    notify(e.message ?? 'That link cannot be opened.');
  }
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
  final join = await sheet<(String, String)>(
    context,
    (context) => _GroupPrompt(swap: current != null),
  );

  if (join == null) {
    return;
  }

  final (raw, given) = join;
  final hex = raw.toLowerCase().padLeft(4, '0');
  final label = given.trim();

  if (raw.isEmpty) {
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

class _GroupPrompt extends StatefulWidget {
  const _GroupPrompt({required this.swap});

  final bool swap;

  @override
  State<_GroupPrompt> createState() => _GroupPromptState();
}

class _GroupPromptState extends State<_GroupPrompt> {
  final _id = TextEditingController();
  final _name = TextEditingController();

  @override
  void dispose() {
    _id.dispose();
    _name.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        sheetTitle(widget.swap ? 'Swap group' : 'Join a group'),
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
            controller: _id,
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
            controller: _name,
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
          onTap: () => Navigator.of(context).pop((_id.text, _name.text)),
        ),
      ],
    );
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
