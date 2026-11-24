import 'dart:async';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../links.dart';
import '../reliquary.dart';
import '../theme.dart';
import '../voice.dart';
import '../widgets/marks.dart';
import '../vigil.dart' show flockOf, localOf;
import 'desk.dart';
import 'join.dart';

part 'talk.dart';

class DesktopApp extends StatelessWidget {
  const DesktopApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Vigil',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        brightness: Brightness.dark,
        useMaterial3: true,
        splashFactory: NoSplash.splashFactory,
        scaffoldBackgroundColor: Palette.bg,
        canvasColor: Palette.bg,
        hoverColor: const Color(0x00000000),
        colorScheme: const ColorScheme.dark(
          primary: Palette.amber,
          onPrimary: Palette.bg,
          secondary: Palette.amber2,
          surface: Palette.bg2,
          onSurface: Palette.fg2,
        ),
        textSelectionTheme: const TextSelectionThemeData(
          cursorColor: Palette.amber,
          selectionColor: Color(0x80e8a33d),
        ),
        textTheme: const TextTheme(bodyMedium: Styles.body),
        tooltipTheme: const TooltipThemeData(
          decoration: BoxDecoration(color: Palette.bg3),
          textStyle: TextStyle(fontSize: 12, color: Palette.fg),
        ),
        popupMenuTheme: const PopupMenuThemeData(
          color: Palette.bg3,
          textStyle: TextStyle(fontSize: 13.5, color: Palette.fg),
        ),
        dialogTheme: const DialogThemeData(backgroundColor: Palette.bg2),
      ),
      builder: (context, child) => Material(
        color: Palette.bg,
        child: DefaultTextStyle(style: Styles.body, child: child!),
      ),
      home: const _Root(),
    );
  }
}

class _Root extends StatefulWidget {
  const _Root();

  @override
  State<_Root> createState() => _RootState();
}

class _RootState extends State<_Root> with WidgetsBindingObserver {
  bool _unlocked = false;
  bool _busy = false;
  String _error = '';

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    WidgetsBinding.instance.addPostFrameCallback((_) => _unlock());
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    unawaited(
      Desk.instance
          .focus(state == AppLifecycleState.resumed)
          .catchError((_) {}),
    );
  }

  Future<void> _unlock() async {
    if (_busy) {
      return;
    }

    setState(() {
      _busy = true;
      _error = '';
    });

    try {
      await Desk.instance.unlock();
      _unlocked = true;
    } on DeskError catch (e) {
      _error = e.cancelled ? '' : e.message;
    }

    if (mounted) {
      setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    if (!_unlocked) {
      return _Locked(busy: _busy, error: _error, onUnlock: _unlock);
    }

    return ValueListenableBuilder<int>(
      valueListenable: Desk.instance.changes,
      builder: (context, _, _) {
        if (!Desk.instance.linked && Desk.instance.chats.isEmpty) {
          return const _Link();
        }

        return const _Shell();
      },
    );
  }
}

class _Locked extends StatelessWidget {
  const _Locked({
    required this.busy,
    required this.error,
    required this.onUnlock,
  });

  final bool busy;
  final String error;
  final VoidCallback onUnlock;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          const WordmarkSquare(size: 28),
          const SizedBox(height: 18),
          Text('VIGIL', style: Styles.wordmark),
          const SizedBox(height: 28),
          if (busy)
            Text('OPENING', style: Styles.rubric)
          else
            _Button(label: 'Unlock', onTap: onUnlock),
          if (error.isNotEmpty) ...[
            const SizedBox(height: 16),
            Text(error, style: const TextStyle(color: Palette.amber)),
          ],
        ],
      ),
    );
  }
}

class _Link extends StatefulWidget {
  const _Link();

  @override
  State<_Link> createState() => _LinkState();
}

class _LinkState extends State<_Link> {
  final _words = TextEditingController();
  final _api = TextEditingController(text: reliquary);
  final _paste = TextEditingController();

  Set<String> _list = const {};
  Join? _join;
  String _step = '';
  String _error = '';
  bool _other = false;

  @override
  void initState() {
    super.initState();
    _words.addListener(() => setState(() {}));

    Join.words().then((list) {
      if (mounted) {
        setState(() => _list = list);
      }
    }, onError: (_) {});
  }

  @override
  void dispose() {
    _join?.cancelled = true;
    _words.dispose();
    _api.dispose();
    _paste.dispose();
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

  Future<void> _start() async {
    final typed = _typed;

    if (typed.length != wordCount || _unknown.isNotEmpty) {
      setState(
        () => _error = 'Type all $wordCount secret words you were given.',
      );
      return;
    }

    final join = Join(_api.text, (step) {
      if (mounted) {
        setState(() => _step = step);
      }
    });

    setState(() {
      _join = join;
      _error = '';
    });

    try {
      await join.run(typed);
      _words.clear();
    } on DeskError catch (e) {
      if (mounted && !join.cancelled) {
        setState(() => _error = e.message);
      }
    }

    if (mounted) {
      setState(() {
        _join = null;
        _step = '';
      });
    }
  }

  void _stop() {
    _join?.cancelled = true;
  }

  Future<void> _run(Future<void> Function() what) async {
    setState(() => _error = '');

    try {
      await what();
      _paste.clear();
    } on DeskError catch (e) {
      setState(() => _error = e.message);
    }
  }

  @override
  Widget build(BuildContext context) {
    final busy = _join != null;
    final unknown = _unknown;

    return Center(
      child: SingleChildScrollView(
        padding: const EdgeInsets.symmetric(vertical: 40),
        child: SizedBox(
          width: 560,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  const WordmarkSquare(),
                  const SizedBox(width: 12),
                  Text('VIGIL', style: Styles.wordmark),
                ],
              ),
              const SizedBox(height: 28),
              Text('Link this computer', style: Styles.title),
              const SizedBox(height: 14),
              const Text(
                'In Covenant, make a new flock and add this computer to it '
                'first, as Device far away in Companion mode. Covenant then '
                'gives you secret words for this computer. Add your phone '
                'after it.',
              ),
              const SizedBox(height: 24),
              Text(
                'SECRET WORDS · ${_typed.length} OF $wordCount',
                style: Styles.rubric,
              ),
              const SizedBox(height: 8),
              _Field(controller: _words, hint: 'word word word ...', lines: 4),
              if (unknown.isNotEmpty)
                Padding(
                  padding: const EdgeInsets.only(top: 6),
                  child: Text(
                    'Not a secret word: ${unknown.join(', ')}',
                    style: const TextStyle(color: Palette.amber),
                  ),
                ),
              const SizedBox(height: 12),
              if (_other) ...[
                Text('RELIQUARY', style: Styles.rubric),
                const SizedBox(height: 8),
                _Field(controller: _api, hint: reliquary, mono: true),
                const SizedBox(height: 12),
              ],
              Row(
                children: [
                  if (busy)
                    _Button(label: 'Stop', ghost: true, onTap: _stop)
                  else
                    _Button(label: 'Join', onTap: _start),
                  const SizedBox(width: 16),
                  Expanded(
                    child: busy
                        ? Text(_step, style: Styles.detail)
                        : _Clickable(
                            onTap: () => setState(() => _other = !_other),
                            child: Text(
                              _other
                                  ? 'Use the default Reliquary'
                                  : 'Use another Reliquary',
                              style: Styles.detail,
                            ),
                          ),
                  ),
                ],
              ),
              const SizedBox(height: 32),
              Text('OR USE A CONFIGURATION FILE', style: Styles.rubric),
              const SizedBox(height: 8),
              _Field(
                controller: _paste,
                hint: 'Paste it here, or open the file',
                lines: 6,
                mono: true,
              ),
              const SizedBox(height: 12),
              Row(
                children: [
                  _Button(
                    label: 'Link',
                    ghost: true,
                    onTap: () => _run(
                      () => Desk.instance.pasteConfig(_paste.text.trim()),
                    ),
                  ),
                  const SizedBox(width: 10),
                  _Button(
                    label: 'Open file',
                    ghost: true,
                    onTap: () => _run(Desk.instance.importConfig),
                  ),
                ],
              ),
              if (_error.isNotEmpty) ...[
                const SizedBox(height: 16),
                Text(_error, style: const TextStyle(color: Palette.amber)),
              ],
            ],
          ),
        ),
      ),
    );
  }
}

const _railWidth = 72.0;
const _topicsWidth = 228.0;

class _Shell extends StatefulWidget {
  const _Shell();

  @override
  State<_Shell> createState() => _ShellState();
}

class _ShellState extends State<_Shell> {
  final Map<String, String> _topics = {};
  bool _favorites = false;
  bool _members = true;
  String _problem = '';
  Timer? _clear;

  @override
  void initState() {
    super.initState();
    Desk.instance.problems.addListener(_problemShown);
  }

  @override
  void dispose() {
    Desk.instance.problems.removeListener(_problemShown);
    _clear?.cancel();
    super.dispose();
  }

  void _problemShown() {
    final text = Desk.instance.problems.value;

    if (text.isEmpty) {
      return;
    }

    Desk.instance.problems.value = '';
    _say(text);
  }

  void _say(String text) {
    setState(() => _problem = text);

    _clear?.cancel();
    _clear = Timer(const Duration(seconds: 6), () {
      if (mounted) {
        setState(() => _problem = '');
      }
    });
  }

  void _select(Chat chat, String topic) {
    setState(() => _favorites = false);
    unawaited(Desk.instance.select(chat, topic).catchError((_) {}));
    unawaited(Desk.instance.digests(chat).catchError((_) {}));
  }

  Future<void> _act(Future<void> Function() what) async {
    try {
      await what();
    } on DeskError catch (e) {
      _say(e.message);
    }
  }

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<int>(
      valueListenable: Desk.instance.changes,
      builder: (context, _, _) => _build(context),
    );
  }

  Widget _build(BuildContext context) {
    final desk = Desk.instance;
    final open = desk.open;
    final chat = open == null ? null : desk.chats[open];

    if (chat != null) {
      _topics[chat.id] = desk.openTopic;
    }

    return LayoutBuilder(
      builder: (context, box) => _layout(chat, box.maxWidth >= 960),
    );
  }

  void _open(Chat chat) {
    final topic = _topics[chat.id] ?? '';

    if (chat.named.any((t) => t.id == topic)) {
      _select(chat, topic);
      return;
    }

    _select(chat, '');
  }

  Widget _layout(Chat? chat, bool wide) {
    final desk = Desk.instance;
    final shown = _favorites ? null : chat;
    final group = shown != null && shown.isGroup && !shown.closed;

    Widget main = const _Empty();

    if (_favorites) {
      main = _Favorites(
        onOpen: (chat, topic) => _select(chat, topic),
        onClose: () => setState(() => _favorites = false),
      );
    } else if (shown != null) {
      main = _Conversation(
        key: ValueKey('${shown.id}/${desk.openTopic}'),
        chat: shown,
        topic: desk.openTopic,
        members: _members,
        onMembers: group && wide
            ? () => setState(() => _members = !_members)
            : null,
        onProblem: _say,
      );
    }

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        _TopBar(
          favorites: _favorites,
          onFavorites: () => setState(() => _favorites = !_favorites),
          act: _act,
        ),
        const ColoredBox(color: Palette.line, child: SizedBox(height: 1)),
        Expanded(
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              SizedBox(
                width: chat == null ? _railWidth : _railWidth + _topicsWidth,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Expanded(
                      child: Row(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          SizedBox(
                            width: _railWidth,
                            child: _Rail(
                              open: chat?.id,
                              onOpen: _open,
                              onSelect: _select,
                              act: _act,
                            ),
                          ),
                          if (chat != null)
                            Expanded(
                              child: _TopicList(
                                chat: chat,
                                current: _favorites ? null : desk.openTopic,
                                onPick: (topic) => _select(chat, topic),
                                act: _act,
                              ),
                            ),
                        ],
                      ),
                    ),
                    const ColoredBox(
                      color: Palette.line,
                      child: SizedBox(height: 1),
                    ),
                    _Me(wide: chat != null),
                  ],
                ),
              ),
              const ColoredBox(color: Palette.line, child: SizedBox(width: 1)),
              Expanded(
                child: Stack(
                  children: [
                    Positioned.fill(child: main),
                    Positioned(
                      top: 64,
                      right: 20,
                      child: _CallPanel(act: _act),
                    ),
                    if (_problem.isNotEmpty)
                      Positioned(
                        left: 20,
                        right: 20,
                        bottom: 90,
                        child: Center(child: _Toast(text: _problem)),
                      ),
                  ],
                ),
              ),
              if (group && wide && _members) ...[
                const ColoredBox(
                  color: Palette.line,
                  child: SizedBox(width: 1),
                ),
                _Members(chat: shown, onOpen: _open),
              ],
            ],
          ),
        ),
      ],
    );
  }
}

class _Toast extends StatelessWidget {
  const _Toast({required this.text});

  final String text;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
      decoration: BoxDecoration(
        color: Palette.bg3,
        border: Border.all(color: Palette.amberDim),
        borderRadius: const BorderRadius.all(Radius.circular(6)),
      ),
      child: Text(text, style: const TextStyle(color: Palette.fg)),
    );
  }
}

class _TopBar extends StatelessWidget {
  const _TopBar({
    required this.favorites,
    required this.onFavorites,
    required this.act,
  });

  final bool favorites;
  final VoidCallback onFavorites;
  final Future<void> Function(Future<void> Function()) act;

  @override
  Widget build(BuildContext context) {
    return ColoredBox(
      color: Palette.well,
      child: SizedBox(
        height: 44,
        child: Row(
          children: [
            const SizedBox(
              width: _railWidth,
              child: Center(child: WordmarkSquare()),
            ),
            Text('VIGIL', style: Styles.wordmark),
            const SizedBox(width: 28),
            Expanded(
              child: Align(
                alignment: Alignment.centerLeft,
                child: ConstrainedBox(
                  constraints: const BoxConstraints(maxWidth: 420),
                  child: _PhoneState(),
                ),
              ),
            ),
            _Icon(
              icon: GlyphKind.star,
              tip: 'Favorites',
              lit: favorites,
              onTap: onFavorites,
            ),
            _Icon(
              icon: GlyphKind.grip,
              tip: 'Settings',
              onTap: () => showDialog<void>(
                context: context,
                builder: (_) => _SettingsDialog(act: act),
              ),
            ),
            const SizedBox(width: 8),
          ],
        ),
      ),
    );
  }
}

class _Rail extends StatelessWidget {
  const _Rail({
    required this.open,
    required this.onOpen,
    required this.onSelect,
    required this.act,
  });

  final String? open;
  final void Function(Chat) onOpen;
  final void Function(Chat, String) onSelect;
  final Future<void> Function(Future<void> Function()) act;

  @override
  Widget build(BuildContext context) {
    final chats = Desk.instance.sorted;
    final stars = chats.where((c) => c.starred).length;

    return ColoredBox(
      color: Palette.well,
      child: Column(
        children: [
          Expanded(
            child: ScrollConfiguration(
              behavior: ScrollConfiguration.of(
                context,
              ).copyWith(scrollbars: false),
              child: ListView(
                padding: const EdgeInsets.symmetric(vertical: 8),
                children: [
                  for (final (i, chat) in chats.indexed) ...[
                    if (i == stars && i > 0)
                      const Center(
                        child: Padding(
                          padding: EdgeInsets.symmetric(vertical: 6),
                          child: ColoredBox(
                            color: Palette.line2,
                            child: SizedBox(width: 32, height: 2),
                          ),
                        ),
                      ),
                    _RailItem(
                      key: ValueKey(chat.id),
                      chat: chat,
                      active: chat.id == open,
                      onTap: () => onOpen(chat),
                      act: act,
                    ),
                  ],
                ],
              ),
            ),
          ),
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 8),
            child: _Icon(
              icon: GlyphKind.plus,
              tip: 'New chat',
              boxed: true,
              onTap: () => _newChat(context, onSelect, act),
            ),
          ),
        ],
      ),
    );
  }
}

class _RailItem extends StatefulWidget {
  const _RailItem({
    super.key,
    required this.chat,
    required this.active,
    required this.onTap,
    required this.act,
  });

  final Chat chat;
  final bool active;
  final VoidCallback onTap;
  final Future<void> Function(Future<void> Function()) act;

  @override
  State<_RailItem> createState() => _RailItemState();
}

class _RailItemState extends State<_RailItem> {
  bool _hover = false;

  double get _mark {
    if (widget.active) {
      return 36;
    }

    if (_hover) {
      return 18;
    }

    if (widget.chat.unreadAll > 0) {
      return 8;
    }

    return 0;
  }

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final chat = widget.chat;
    final unread = chat.unreadAll;

    return _RightClick(
      onMenu: (at) => unawaited(_chatMenu(context, chat, at, widget.act)),
      child: _Hint(
        title: desk.title(chat),
        detail: desk.title(chat) == chat.id ? '' : chat.flock,
        child: MouseRegion(
          cursor: SystemMouseCursors.click,
          onEnter: (_) => setState(() => _hover = true),
          onExit: (_) => setState(() => _hover = false),
          child: GestureDetector(
            behavior: HitTestBehavior.opaque,
            onTap: widget.onTap,
            child: SizedBox(
              height: 54,
              child: Stack(
                alignment: Alignment.center,
                children: [
                  Positioned(
                    left: 0,
                    child: Container(
                      width: 4,
                      height: _mark,
                      decoration: BoxDecoration(
                        color: widget.active ? Palette.amber : Palette.fg2,
                        borderRadius: const BorderRadius.horizontal(
                          right: Radius.circular(4),
                        ),
                      ),
                    ),
                  ),
                  Opacity(
                    opacity: chat.closed ? 0.5 : 1,
                    child: _Avatar(
                      peer: chat.isGroup ? '' : chat.id,
                      label: desk.title(chat),
                      group: chat.isGroup,
                      size: 44,
                      presence: !chat.isGroup,
                    ),
                  ),
                  if (chat.silenced)
                    const Positioned(
                      left: 10,
                      bottom: 2,
                      child: _RailMark(
                        child: Glyph(
                          GlyphKind.snoozed,
                          size: 12,
                          color: Palette.dim,
                        ),
                      ),
                    ),
                  if (unread > 0)
                    Positioned(
                      right: 8,
                      top: 1,
                      child: _RailMark(
                        child: _Unread(
                          count: unread,
                          quiet: chat.silenced,
                          margin: EdgeInsets.zero,
                        ),
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
}

class _Hint extends StatefulWidget {
  const _Hint({required this.title, required this.detail, required this.child});

  final String title;
  final String detail;
  final Widget child;

  @override
  State<_Hint> createState() => _HintState();
}

class _HintState extends State<_Hint> {
  final _portal = OverlayPortalController();
  final _link = LayerLink();

  Widget _card(BuildContext context) {
    return Positioned(
      left: 0,
      top: 0,
      child: CompositedTransformFollower(
        link: _link,
        showWhenUnlinked: false,
        targetAnchor: Alignment.centerRight,
        followerAnchor: Alignment.centerLeft,
        offset: const Offset(-8, 0),
        child: IgnorePointer(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 300),
            child: CustomPaint(
              painter: const _HintPainter(),
              child: Padding(
                padding: const EdgeInsets.fromLTRB(22, 9, 16, 9),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      widget.title,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        fontSize: 14.5,
                        height: 1.3,
                        fontWeight: FontWeight.w600,
                        color: Palette.fg,
                      ),
                    ),
                    if (widget.detail.isNotEmpty)
                      Text(
                        widget.detail,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: Styles.detail,
                      ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Semantics(
      label: widget.title,
      child: CompositedTransformTarget(
        link: _link,
        child: OverlayPortal(
          controller: _portal,
          overlayChildBuilder: _card,
          child: Listener(
            onPointerDown: (_) => _portal.hide(),
            child: MouseRegion(
              onEnter: (_) => _portal.show(),
              onExit: (_) => _portal.hide(),
              child: widget.child,
            ),
          ),
        ),
      ),
    );
  }
}

class _HintPainter extends CustomPainter {
  const _HintPainter();

  @override
  void paint(Canvas canvas, Size size) {
    const arrow = 7.0;
    final y = size.height / 2;
    final body = Path()
      ..addRRect(
        RRect.fromLTRBR(
          arrow,
          0,
          size.width,
          size.height,
          const Radius.circular(8),
        ),
      );
    final point = Path()
      ..moveTo(arrow + 1, y - arrow)
      ..lineTo(0, y)
      ..lineTo(arrow + 1, y + arrow)
      ..close();
    final path = Path.combine(PathOperation.union, body, point);

    canvas.drawShadow(path, const Color(0xff000000), 6, false);
    canvas.drawPath(path, Paint()..color = Palette.bg3);
    canvas.drawPath(
      path,
      Paint()
        ..color = Palette.line2
        ..style = PaintingStyle.stroke,
    );
  }

  @override
  bool shouldRepaint(_HintPainter old) => false;
}

class _RailMark extends StatelessWidget {
  const _RailMark({required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(2),
      decoration: const BoxDecoration(
        color: Palette.well,
        borderRadius: BorderRadius.all(Radius.circular(10)),
      ),
      child: child,
    );
  }
}

class _TopicList extends StatelessWidget {
  const _TopicList({
    required this.chat,
    required this.current,
    required this.onPick,
    required this.act,
  });

  final Chat chat;
  final String? current;
  final void Function(String) onPick;
  final Future<void> Function(Future<void> Function()) act;

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;

    return ColoredBox(
      color: Palette.bg2,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          SizedBox(
            height: 56,
            child: _RightClick(
              onMenu: (at) => unawaited(_chatMenu(context, chat, at, act)),
              child: _Clickable(
                onTap: () => unawaited(_details(context, chat)),
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 16),
                  child: Align(
                    alignment: Alignment.centerLeft,
                    child: Text(
                      desk.title(chat),
                      style: Styles.barTitle,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                    ),
                  ),
                ),
              ),
            ),
          ),
          const ColoredBox(color: Palette.line, child: SizedBox(height: 1)),
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 8, 8, 2),
            child: SizedBox(
              height: 34,
              child: Row(
                children: [
                  Expanded(child: Text('TOPICS', style: Styles.rubric)),
                  if (desk.owns(chat) && !chat.closed)
                    _Icon(
                      icon: GlyphKind.plus,
                      tip: 'New topic',
                      onTap: () => _newTopic(context, chat, act),
                    ),
                ],
              ),
            ),
          ),
          Expanded(
            child: ListView(
              padding: const EdgeInsets.only(bottom: 12),
              children: [
                _TopicRow(
                  chat: chat,
                  active: current == '',
                  onTap: () => onPick(''),
                ),
                for (final topic in chat.named)
                  _RightClick(
                    onMenu: (at) => unawaited(
                      _topicMenu(context, chat, topic, at, act),
                    ),
                    child: _TopicRow(
                      chat: chat,
                      topic: topic,
                      active: current == topic.id,
                      onTap: () => onPick(topic.id),
                    ),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _TopicRow extends StatefulWidget {
  const _TopicRow({
    required this.chat,
    required this.active,
    required this.onTap,
    this.topic,
  });

  final Chat chat;
  final Topic? topic;
  final bool active;
  final VoidCallback onTap;

  @override
  State<_TopicRow> createState() => _TopicRowState();
}

class _TopicRowState extends State<_TopicRow> {
  bool _hover = false;

  @override
  Widget build(BuildContext context) {
    final chat = widget.chat;
    final topic = widget.topic;
    final id = topic?.id ?? '';
    final unread = chat.unreadIn(id);
    final typing = Desk.instance.typers(chat, id).isNotEmpty;

    var color = Palette.dim;

    if (unread > 0 || _hover) {
      color = Palette.fg2;
    }

    if (widget.active) {
      color = Palette.fg;
    }

    var bg = const Color(0x00000000);

    if (_hover) {
      bg = Palette.bg3;
    }

    if (widget.active) {
      bg = Palette.amberWash;
    }

    return MouseRegion(
      cursor: SystemMouseCursors.click,
      onEnter: (_) => setState(() => _hover = true),
      onExit: (_) => setState(() => _hover = false),
      child: GestureDetector(
        onTap: widget.onTap,
        child: Container(
          height: 34,
          margin: const EdgeInsets.symmetric(horizontal: 8, vertical: 1),
          padding: const EdgeInsets.symmetric(horizontal: 10),
          decoration: BoxDecoration(
            color: bg,
            borderRadius: const BorderRadius.all(Radius.circular(8)),
          ),
          child: Row(
            children: [
              _Hash(topic: topic),
              const SizedBox(width: 9),
              Expanded(
                child: Text(
                  topic?.name ?? 'Main',
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontSize: 14,
                    height: 1.3,
                    fontWeight: unread > 0 ? FontWeight.w600 : FontWeight.w500,
                    color: color,
                  ),
                ),
              ),
              if (typing)
                const Padding(
                  padding: EdgeInsets.only(left: 8),
                  child: Text(
                    'typing…',
                    style: TextStyle(
                      fontSize: 12,
                      height: 1.3,
                      fontStyle: FontStyle.italic,
                      color: Palette.amber,
                    ),
                  ),
                ),
              if (unread > 0 && !widget.active)
                _Unread(count: unread, quiet: chat.silenced),
            ],
          ),
        ),
      ),
    );
  }
}

class _Hash extends StatelessWidget {
  const _Hash({required this.topic, this.size = 15});

  final Topic? topic;
  final double size;

  @override
  Widget build(BuildContext context) {
    final topic = this.topic;

    return Text(
      '#',
      style: TextStyle(
        fontFamily: Fonts.mono,
        fontSize: size,
        height: 1.2,
        fontWeight: FontWeight.w700,
        color: topic == null ? Palette.dim : _topicColor(topic.color),
      ),
    );
  }
}

class _Me extends StatelessWidget {
  const _Me({required this.wide});

  final bool wide;

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;

    final where = switch (desk.selves.length) {
      0 => 'Not heard from your phone',
      1 => desk.selves.values.first,
      _ => '${desk.selves.length} flocks',
    };

    Future<void> flocks() {
      return showDialog<void>(
        context: context,
        builder: (_) => const _FlocksDialog(),
      );
    }

    return ColoredBox(
      color: Palette.well,
      child: SizedBox(
        height: 60,
        child: Row(
          children: [
            SizedBox(
              width: _railWidth,
              child: _Hint(
                title: desk.selfLabel,
                detail: where,
                child: Center(
                  child: _Clickable(
                    onTap: () => unawaited(flocks()),
                    child: const _Avatar(peer: '00', size: 36),
                  ),
                ),
              ),
            ),
            if (wide) ...[
              Expanded(
                child: Column(
                  mainAxisAlignment: MainAxisAlignment.center,
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      desk.selfLabel,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        fontSize: 13.5,
                        fontWeight: FontWeight.w600,
                        color: Palette.fg,
                      ),
                    ),
                    Text(
                      where,
                      style: Styles.detail,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                    ),
                  ],
                ),
              ),
              _Icon(
                icon: GlyphKind.group,
                tip: 'Your flocks',
                onTap: () => unawaited(flocks()),
              ),
              const SizedBox(width: 8),
            ],
          ],
        ),
      ),
    );
  }
}

class _FlocksDialog extends StatelessWidget {
  const _FlocksDialog();

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<int>(
      valueListenable: Desk.instance.changes,
      builder: (context, _, _) => _build(context),
    );
  }

  Widget _build(BuildContext context) {
    final desk = Desk.instance;
    final flocks = desk.selves.keys.toList()..sort();

    return _Dialog(
      title: 'Your flocks',
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (flocks.isEmpty)
            const Text('Not heard from your phone')
          else
            Row(
              children: [
                Expanded(child: Text('FLOCK', style: Styles.rubric)),
                Text('YOUR ID', style: Styles.rubric),
                const SizedBox(width: 16),
              ],
            ),
          const SizedBox(height: 6),
          ConstrainedBox(
            constraints: const BoxConstraints(maxHeight: 360),
            child: ListView(
              shrinkWrap: true,
              padding: const EdgeInsets.only(right: 16),
              children: [
                for (final flock in flocks)
                  Padding(
                    padding: const EdgeInsets.symmetric(vertical: 7),
                    child: Row(
                      children: [
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Text(
                                flock,
                                style: Styles.detail.copyWith(
                                  color: Palette.fg,
                                ),
                              ),
                              if (desk.selfSigned(flock).isNotEmpty)
                                Text(
                                  desk.selfSigned(flock),
                                  style: Styles.detail,
                                  overflow: TextOverflow.ellipsis,
                                ),
                            ],
                          ),
                        ),
                        const SizedBox(width: 12),
                        SelectableText(
                          desk.selfIn(flock),
                          style: Styles.detail.copyWith(color: Palette.amber),
                        ),
                      ],
                    ),
                  ),
              ],
            ),
          ),
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

class _PhoneState extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;

    var busy = false;
    var text = 'WAITING FOR YOUR PHONE';
    var color = Palette.amber;

    if (!desk.linked) {
      text = 'NOT LINKED';
      color = Palette.dim;
    } else if (desk.outdated == 'phone') {
      text = 'UPDATE VIGIL ON YOUR PHONE';
    } else if (desk.outdated == 'desktop') {
      text = 'UPDATE THIS APP';
    } else if (desk.online && desk.sync == 'unsynced') {
      text = 'WAITING FOR A FULL SYNC';
    } else if (desk.online && desk.sync == 'syncing') {
      text = 'SYNCING YOUR CHATS';
      busy = true;
    } else if (desk.online && desk.behind > 0) {
      text = 'CATCHING UP';
      busy = true;
    } else if (desk.online) {
      text = 'PHONE ONLINE · ${desk.link.toUpperCase()}';
      color = Palette.good;
    }

    if (busy && desk.behind > 0) {
      text = '$text · ${desk.behind} TO GO';
    }

    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            _Dot(color: color),
            const SizedBox(width: 10),
            Flexible(
              child: Text(
                text,
                style: Styles.rubric.copyWith(color: color),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
              ),
            ),
            if (desk.online && desk.queued > 0) ...[
              const SizedBox(width: 16),
              Text('${desk.queued} WAITING', style: Styles.rubric),
            ],
          ],
        ),
        if (busy)
          const Padding(
            padding: EdgeInsets.only(top: 6),
            child: LinearProgressIndicator(
              minHeight: 2,
              color: Palette.amber,
              backgroundColor: Palette.amberDim,
            ),
          ),
      ],
    );
  }
}

class _Dot extends StatelessWidget {
  const _Dot({required this.color, this.size = 7});

  final Color color;
  final double size;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(color: color, shape: BoxShape.circle),
    );
  }
}

Future<void> _chatMenu(
  BuildContext context,
  Chat chat,
  Offset at,
  Future<void> Function(Future<void> Function()) act,
) async {
  final desk = Desk.instance;
  final owner = chat.owner == desk.selfIn(chat.id);

  final what = await _popup(context, at, [
    _item('details', 'Details'),
    _item('star', chat.starred ? 'Unstar' : 'Star'),
    if (chat.silenced)
      _item('unmute', 'Unmute')
    else ...[
      _item('mute1', 'Mute for an hour'),
      _item('mute8', 'Mute for 8 hours'),
      _item('mute', 'Mute'),
    ],
    if (desk.owns(chat) && !chat.closed) _item('topic', 'New topic'),
    if (chat.isGroup && desk.owns(chat)) _item('edit', 'Edit group'),
    if (chat.isGroup && !chat.closed)
      _item('leave', owner ? 'End group' : 'Leave group'),
    _item('delete', 'Delete chat'),
  ]);

  if (what == null || !context.mounted) {
    return;
  }

  final now = DateTime.now().millisecondsSinceEpoch;

  switch (what) {
    case 'details':
      await _details(context, chat);
    case 'star':
      await act(() => desk.star(chat, !chat.starred));
    case 'unmute':
      await act(() => desk.mute(chat, 0));
    case 'mute1':
      await act(() => desk.mute(chat, now + 3600 * 1000));
    case 'mute8':
      await act(() => desk.mute(chat, now + 8 * 3600 * 1000));
    case 'mute':
      await act(() => desk.mute(chat, -1));
    case 'topic':
      await _newTopic(context, chat, act);
    case 'edit':
      await showDialog<void>(
        context: context,
        builder: (_) => _GroupDialog(chat: chat, act: act),
      );
    case 'leave':
      final sure = await _confirm(
        context,
        owner ? 'End this group?' : 'Leave this group?',
        'Your phone does this for you, the messages here stay.',
        owner ? 'End' : 'Leave',
      );
      if (sure) {
        await act(() => desk.leaveGroup(chat));
      }
    case 'delete':
      final sure = await _confirm(
        context,
        'Delete this chat?',
        'Your phone deletes it too.',
        'Delete',
      );
      if (sure) {
        await act(() => desk.delete(chat));
      }
  }
}

class _Unread extends StatelessWidget {
  const _Unread({
    required this.count,
    this.quiet = false,
    this.margin = const EdgeInsets.only(left: 8),
  });

  final int count;
  final bool quiet;
  final EdgeInsets margin;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: margin,
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
      decoration: BoxDecoration(
        color: quiet ? Palette.line2 : Palette.amber,
        borderRadius: const BorderRadius.all(Radius.circular(8)),
      ),
      child: Text(
        count > 99 ? '99+' : '$count',
        style: TextStyle(
          fontSize: 11,
          fontWeight: FontWeight.w700,
          color: quiet ? Palette.fg2 : Palette.bg,
        ),
      ),
    );
  }
}

class _Empty extends StatelessWidget {
  const _Empty();

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          const WordmarkSquare(size: 22),
          const SizedBox(height: 16),
          Text('PICK A CHAT', style: Styles.rubric),
        ],
      ),
    );
  }
}

class _Favorites extends StatelessWidget {
  const _Favorites({required this.onOpen, required this.onClose});

  final void Function(Chat, String) onOpen;
  final VoidCallback onClose;

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final list = desk.favorites;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        SizedBox(
          height: 56,
          child: Row(
            children: [
              const SizedBox(width: 20),
              const Glyph(GlyphKind.star, size: 20, color: Palette.amber),
              const SizedBox(width: 12),
              Expanded(child: Text('Favorites', style: Styles.barTitle)),
              _Icon(icon: GlyphKind.close, tip: 'Close', onTap: onClose),
              const SizedBox(width: 12),
            ],
          ),
        ),
        const ColoredBox(color: Palette.line, child: SizedBox(height: 1)),
        Expanded(
          child: list.isEmpty
              ? Center(child: Text('NO FAVORITES YET', style: Styles.rubric))
              : ListView.builder(
                  padding: const EdgeInsets.symmetric(vertical: 12),
                  itemCount: list.length,
                  itemBuilder: (context, i) {
                    final message = list[i];
                    final chat = desk.chatOf(message);

                    return _Clickable(
                      onTap: () {
                        if (chat != null) {
                          onOpen(chat, message.topic);
                        }
                      },
                      child: Padding(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 20,
                          vertical: 8,
                        ),
                        child: Row(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            _Avatar(peer: message.author, size: 32),
                            const SizedBox(width: 12),
                            Expanded(
                              child: Column(
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  Text(
                                    '${desk.author(message)} · '
                                    '${chat == null ? '' : desk.title(chat)} '
                                    '· ${_when(message.at)}',
                                    style: Styles.detail,
                                  ),
                                  const SizedBox(height: 2),
                                  Text(
                                    message.summary,
                                    style: const TextStyle(
                                      fontSize: 14,
                                      color: Palette.fg2,
                                    ),
                                  ),
                                ],
                              ),
                            ),
                          ],
                        ),
                      ),
                    );
                  },
                ),
        ),
      ],
    );
  }
}

Future<void> _newChat(
  BuildContext context,
  void Function(Chat, String) onSelect,
  Future<void> Function(Future<void> Function()) act,
) async {
  final picked = await showDialog<String>(
    context: context,
    builder: (_) => const _NewChatDialog(),
  );

  if (picked == null || !context.mounted) {
    return;
  }

  if (picked == 'group') {
    await showDialog<void>(
      context: context,
      builder: (_) => _GroupDialog(act: act),
    );
    return;
  }

  onSelect(Desk.instance.chat(picked), '');
}

Future<void> _newTopic(
  BuildContext context,
  Chat chat,
  Future<void> Function(Future<void> Function()) act, {
  Topic? topic,
}) async {
  final picked = await showDialog<(String, int)>(
    context: context,
    builder: (_) => _TopicDialog(topic: topic),
  );

  if (picked == null) {
    return;
  }

  await act(
    () => Desk.instance.topic(chat, picked.$1, picked.$2, id: topic?.id ?? ''),
  );
}

class _NewChatDialog extends StatelessWidget {
  const _NewChatDialog();

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final known = desk.known;

    return _Dialog(
      title: 'New chat',
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (known.isEmpty)
            const Padding(
              padding: EdgeInsets.symmetric(vertical: 12),
              child: Text(
                'Nobody is around yet. Peers show up here once your phone '
                'hears from them.',
              ),
            ),
          ConstrainedBox(
            constraints: const BoxConstraints(maxHeight: 360),
            child: ListView(
              shrinkWrap: true,
              padding: const EdgeInsets.only(right: 16),
              children: [
                for (final peer in known)
                  _Clickable(
                    onTap: () => Navigator.of(context).pop(peer),
                    child: Padding(
                      padding: const EdgeInsets.symmetric(vertical: 7),
                      child: Row(
                        children: [
                          _Avatar(peer: peer, size: 30, presence: true),
                          const SizedBox(width: 12),
                          Expanded(
                            child: Text(
                              desk.name(peer),
                              style: const TextStyle(color: Palette.fg),
                            ),
                          ),
                          Text(peer, style: Styles.detail),
                        ],
                      ),
                    ),
                  ),
              ],
            ),
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              _Button(
                label: 'New group',
                ghost: true,
                onTap: () => Navigator.of(context).pop('group'),
              ),
              const Spacer(),
              _Button(
                label: 'Cancel',
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

class _GroupDialog extends StatefulWidget {
  const _GroupDialog({this.chat, required this.act});

  final Chat? chat;
  final Future<void> Function(Future<void> Function()) act;

  @override
  State<_GroupDialog> createState() => _GroupDialogState();
}

class _GroupDialogState extends State<_GroupDialog> {
  final _name = TextEditingController();
  final Set<String> _picked = {};

  @override
  void initState() {
    super.initState();

    final chat = widget.chat;
    if (chat != null) {
      _name.text = chat.name;
      _picked.addAll(
        chat.members.where((m) => m != Desk.instance.selfIn(chat.id)),
      );
    }
  }

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  Future<void> _done() async {
    final desk = Desk.instance;
    final name = _name.text.trim();
    final chat = widget.chat;

    if (_picked.isEmpty) {
      return;
    }

    Navigator.of(context).pop();

    if (chat == null) {
      await widget.act(
        () => desk.createGroup(
          flockOf(_picked.first),
          name,
          _picked.toList(),
        ),
      );
    } else {
      await widget.act(() => desk.editGroup(chat, name, _picked.toList()));
    }
  }

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final flock =
        widget.chat?.flock ??
        (_picked.isEmpty ? null : flockOf(_picked.first));
    final known =
        {...desk.known, ..._picked}
            .where((p) => flock == null || flockOf(p) == flock)
            .toList()
          ..sort((a, b) => desk.name(a).compareTo(desk.name(b)));

    return _Dialog(
      title: widget.chat == null ? 'New group' : 'Edit group',
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Text('NAME', style: Styles.rubric),
          const SizedBox(height: 6),
          _Field(
            controller: _name,
            hint: 'Friday night',
            limit: groupNameMax,
            autofocus: true,
          ),
          const SizedBox(height: 16),
          Text('MEMBERS · ${_picked.length}', style: Styles.rubric),
          const SizedBox(height: 6),
          ConstrainedBox(
            constraints: const BoxConstraints(maxHeight: 300),
            child: ListView(
              shrinkWrap: true,
              padding: const EdgeInsets.only(right: 16),
              children: [
                for (final peer in known)
                  _Clickable(
                    onTap: () => setState(() {
                      if (!_picked.remove(peer)) {
                        _picked.add(peer);
                      }
                    }),
                    child: Padding(
                      padding: const EdgeInsets.symmetric(vertical: 6),
                      child: Row(
                        children: [
                          _Check(on: _picked.contains(peer)),
                          const SizedBox(width: 12),
                          _Avatar(peer: peer, size: 26, presence: true),
                          const SizedBox(width: 10),
                          Expanded(
                            child: Text(
                              desk.name(peer),
                              style: const TextStyle(color: Palette.fg),
                            ),
                          ),
                          Text(peer, style: Styles.detail),
                        ],
                      ),
                    ),
                  ),
              ],
            ),
          ),
          const SizedBox(height: 16),
          Row(
            mainAxisAlignment: MainAxisAlignment.end,
            children: [
              _Button(
                label: 'Cancel',
                ghost: true,
                onTap: () => Navigator.of(context).pop(),
              ),
              const SizedBox(width: 10),
              _Button(
                label: widget.chat == null ? 'Create' : 'Save',
                onTap: _done,
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _Check extends StatelessWidget {
  const _Check({required this.on});

  final bool on;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 16,
      height: 16,
      decoration: BoxDecoration(
        color: on ? Palette.amber : null,
        border: Border.all(color: on ? Palette.amber : Palette.line2),
        borderRadius: const BorderRadius.all(Radius.circular(3)),
      ),
      child: on
          ? const Center(
              child: Text(
                '✓',
                style: TextStyle(fontSize: 11, color: Palette.bg, height: 1),
              ),
            )
          : null,
    );
  }
}

class _TopicDialog extends StatefulWidget {
  const _TopicDialog({this.topic});

  final Topic? topic;

  @override
  State<_TopicDialog> createState() => _TopicDialogState();
}

class _TopicDialogState extends State<_TopicDialog> {
  final _name = TextEditingController();
  int _color = topicColors.first;

  @override
  void initState() {
    super.initState();

    final topic = widget.topic;
    if (topic != null) {
      _name.text = topic.name;
      _color = topic.color;
    }
  }

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  void _done() {
    final name = _name.text.trim();

    if (name.isEmpty) {
      return;
    }

    Navigator.of(context).pop((name, _color));
  }

  @override
  Widget build(BuildContext context) {
    return _Dialog(
      title: widget.topic == null ? 'New topic' : 'Rename topic',
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('NAME', style: Styles.rubric),
          const SizedBox(height: 6),
          _Field(
            controller: _name,
            hint: 'Holiday plans',
            limit: topicNameMax,
            autofocus: true,
            onSubmit: _done,
          ),
          const SizedBox(height: 16),
          Text('COLOR', style: Styles.rubric),
          const SizedBox(height: 8),
          Row(
            children: [
              for (final color in topicColors)
                _Clickable(
                  onTap: () => setState(() => _color = color),
                  child: Container(
                    margin: const EdgeInsets.only(right: 10),
                    padding: const EdgeInsets.all(3),
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      border: Border.all(
                        color: color == _color
                            ? Palette.fg
                            : const Color(0x00000000),
                        width: 2,
                      ),
                    ),
                    child: _Dot(color: _topicColor(color), size: 22),
                  ),
                ),
            ],
          ),
          const SizedBox(height: 20),
          Row(
            mainAxisAlignment: MainAxisAlignment.end,
            children: [
              _Button(
                label: 'Cancel',
                ghost: true,
                onTap: () => Navigator.of(context).pop(),
              ),
              const SizedBox(width: 10),
              _Button(
                label: widget.topic == null ? 'Create' : 'Save',
                onTap: _done,
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _SettingsDialog extends StatelessWidget {
  const _SettingsDialog({required this.act});

  final Future<void> Function(Future<void> Function()) act;

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<int>(
      valueListenable: Desk.instance.changes,
      builder: (context, _, _) => _build(context),
    );
  }

  Widget _build(BuildContext context) {
    final desk = Desk.instance;

    return _Dialog(
      title: 'Settings',
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              _Avatar(peer: '00', size: 56),
              const SizedBox(width: 16),
              _Button(
                label: 'Change picture',
                ghost: true,
                onTap: () => act(() async {
                  await desk.avatar();
                }),
              ),
              const SizedBox(width: 10),
              if (desk.selfAvatar.isNotEmpty)
                _Button(
                  label: 'Remove',
                  ghost: true,
                  onTap: () => act(desk.dropAvatar),
                ),
            ],
          ),
          const SizedBox(height: 12),
          const Text(
            'Your phone keeps this, it changes there too.',
            style: Styles.detail,
          ),
          const SizedBox(height: 24),
          Row(
            children: [
              if (desk.linked) ...[
                _Button(
                  label: 'Unlink',
                  ghost: true,
                  onTap: () async {
                    final sure = await _confirm(
                      context,
                      'Unlink this computer?',
                      'It stops hearing from your phone. The messages it '
                          'has stay.',
                      'Unlink',
                    );

                    if (sure && context.mounted) {
                      Navigator.of(context).pop();
                      await desk.unlink();
                    }
                  },
                ),
                const SizedBox(width: 10),
                _Button(
                  label: 'Resync',
                  ghost: true,
                  onTap: () async {
                    final sure = await _confirm(
                      context,
                      'Resync with your phone?',
                      'Your phone sends all its chats again. This waits '
                          'until your phone is unlocked and can take a while.',
                      'Resync',
                    );

                    if (sure && context.mounted) {
                      Navigator.of(context).pop();
                      await desk.resync();
                    }
                  },
                ),
              ] else
                _Button(
                  label: 'Link',
                  ghost: true,
                  onTap: () {
                    Navigator.of(context).pop();
                    showDialog<void>(
                      context: context,
                      builder: (_) => const Dialog(
                        backgroundColor: Palette.bg,
                        child: Padding(
                          padding: EdgeInsets.all(28),
                          child: _Link(),
                        ),
                      ),
                    );
                  },
                ),
              const Spacer(),
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

class _Dialog extends StatelessWidget {
  const _Dialog({required this.title, required this.child});

  final String title;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Dialog(
      backgroundColor: Palette.bg2,
      shape: const RoundedRectangleBorder(
        side: BorderSide(color: Palette.line2),
        borderRadius: BorderRadius.all(Radius.circular(8)),
      ),
      child: SizedBox(
        width: 440,
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Text(title, style: Styles.cardTitle),
              const SizedBox(height: 16),
              child,
            ],
          ),
        ),
      ),
    );
  }
}

class _Avatar extends StatelessWidget {
  const _Avatar({
    required this.peer,
    this.label,
    this.group = false,
    this.size = 36,
    this.presence = false,
  });

  final String peer;
  final String? label;
  final bool group;
  final double size;
  final bool presence;

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final shown = peer == '00' ? desk.selfLabel : desk.name(peer);
    final text = (label ?? shown).replaceFirst('~', '');
    final words = text.trim().split(RegExp(r'\s+'));
    var initials = words.first.isEmpty ? '?' : words.first[0];
    if (words.length > 1 && words.last.isNotEmpty) {
      initials += words.last[0];
    }

    if (RegExp(r'^[0-9a-f]{1,16}-[0-9a-f]{2}$').hasMatch(text)) {
      initials = localOf(text);
    }

    const tints = [
      Color(0xff5a4220),
      Color(0xff3d4a35),
      Color(0xff4a3540),
      Color(0xff35424a),
      Color(0xff4a4035),
    ];

    final tint = tints[text.codeUnits.fold(0, (a, b) => a + b) % 5];
    final radius = BorderRadius.all(
      Radius.circular(group ? size * 0.28 : size / 2),
    );

    final plain = Container(
      width: size,
      height: size,
      alignment: Alignment.center,
      decoration: BoxDecoration(color: tint, borderRadius: radius),
      child: Text(
        initials.toUpperCase(),
        style: TextStyle(
          fontFamily: Fonts.head,
          fontWeight: FontWeight.w600,
          fontSize: size * 0.38,
          color: Palette.fg,
        ),
      ),
    );

    Widget face = plain;

    if (!group && peer.isNotEmpty) {
      face = FutureBuilder<Uint8List?>(
        future: desk.avatarOf(peer),
        builder: (context, snap) {
          final bytes = snap.data;

          if (bytes == null) {
            return plain;
          }

          return ClipRRect(
            borderRadius: radius,
            child: Image.memory(
              bytes,
              width: size,
              height: size,
              fit: BoxFit.cover,
              gaplessPlayback: true,
              errorBuilder: (_, _, _) => plain,
            ),
          );
        },
      );
    }

    if (!presence || group) {
      return face;
    }

    final dot = _presence(peer);

    if (dot == null) {
      return face;
    }

    return SizedBox(
      width: size,
      height: size,
      child: Stack(
        clipBehavior: Clip.none,
        children: [
          face,
          Positioned(
            right: -1,
            bottom: -1,
            child: Container(
              width: size * 0.32,
              height: size * 0.32,
              decoration: BoxDecoration(
                color: dot,
                shape: BoxShape.circle,
                border: Border.all(color: Palette.bg2, width: 2),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _Clickable extends StatelessWidget {
  const _Clickable({required this.onTap, required this.child});

  final VoidCallback onTap;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return MouseRegion(
      cursor: SystemMouseCursors.click,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: child,
      ),
    );
  }
}

class _Icon extends StatefulWidget {
  const _Icon({
    required this.icon,
    required this.tip,
    required this.onTap,
    this.boxed = false,
    this.lit = false,
  });

  final GlyphKind icon;
  final String tip;
  final VoidCallback onTap;
  final bool boxed;
  final bool lit;

  @override
  State<_Icon> createState() => _IconState();
}

class _IconState extends State<_Icon> {
  bool _hover = false;

  @override
  Widget build(BuildContext context) {
    var color = Palette.dim;

    if (_hover || widget.lit) {
      color = Palette.amber;
    }

    return Tooltip(
      message: widget.tip,
      waitDuration: const Duration(milliseconds: 400),
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        onEnter: (_) => setState(() => _hover = true),
        onExit: (_) => setState(() => _hover = false),
        child: GestureDetector(
          onTap: widget.onTap,
          child: Container(
            width: 34,
            height: 34,
            decoration: BoxDecoration(
              color: widget.boxed || _hover ? Palette.bg3 : null,
              border: widget.boxed ? Border.all(color: Palette.line2) : null,
              borderRadius: const BorderRadius.all(Radius.circular(6)),
            ),
            alignment: Alignment.center,
            child: Glyph(widget.icon, size: 18, color: color),
          ),
        ),
      ),
    );
  }
}

class _Button extends StatefulWidget {
  const _Button({
    required this.label,
    required this.onTap,
    this.ghost = false,
    this.color,
  });

  final String label;
  final VoidCallback onTap;
  final bool ghost;
  final Color? color;

  @override
  State<_Button> createState() => _ButtonState();
}

class _ButtonState extends State<_Button> {
  bool _hover = false;

  @override
  Widget build(BuildContext context) {
    final base = widget.color ?? Palette.amber;
    final fill = widget.ghost
        ? (_hover ? Palette.bg3 : const Color(0x00000000))
        : (_hover ? Color.lerp(base, Palette.fg, 0.15)! : base);

    return MouseRegion(
      cursor: SystemMouseCursors.click,
      onEnter: (_) => setState(() => _hover = true),
      onExit: (_) => setState(() => _hover = false),
      child: GestureDetector(
        onTap: widget.onTap,
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 11),
          decoration: BoxDecoration(
            color: fill,
            border: Border.all(
              color: widget.ghost ? Palette.line2 : fill,
            ),
            borderRadius: const BorderRadius.all(Radius.circular(6)),
          ),
          child: Text(
            widget.label.toUpperCase(),
            style: Styles.btn.copyWith(
              color: widget.ghost ? Palette.fg : Palette.bg,
            ),
          ),
        ),
      ),
    );
  }
}

class _Utf8Limit extends TextInputFormatter {
  const _Utf8Limit(this.max);

  final int max;

  @override
  TextEditingValue formatEditUpdate(
    TextEditingValue oldValue,
    TextEditingValue newValue,
  ) {
    if (utf8Length(newValue.text) <= max) {
      return newValue;
    }

    return oldValue;
  }
}

int utf8Length(String text) {
  var len = 0;

  for (final rune in text.runes) {
    if (rune < 0x80) {
      len += 1;
    } else if (rune < 0x800) {
      len += 2;
    } else if (rune < 0x10000) {
      len += 3;
    } else {
      len += 4;
    }
  }

  return len;
}

class _Field extends StatelessWidget {
  const _Field({
    required this.controller,
    required this.hint,
    this.focus,
    this.lines = 1,
    this.mono = false,
    this.limit = 0,
    this.autofocus = false,
    this.onSubmit,
    this.onChanged,
  });

  final TextEditingController controller;
  final FocusNode? focus;
  final String hint;
  final int lines;
  final bool mono;
  final int limit;
  final bool autofocus;
  final VoidCallback? onSubmit;
  final ValueChanged<String>? onChanged;

  @override
  Widget build(BuildContext context) {
    final style = TextStyle(
      inherit: false,
      fontSize: mono ? 12.5 : 14.5,
      height: 1.45,
      fontFamily: mono ? Fonts.mono : null,
      color: Palette.fg2,
      textBaseline: TextBaseline.alphabetic,
    );

    return TextField(
      controller: controller,
      focusNode: focus,
      autofocus: autofocus,
      minLines: 1,
      maxLines: lines,
      keyboardType: TextInputType.multiline,
      textInputAction: onSubmit == null
          ? TextInputAction.newline
          : TextInputAction.send,
      onSubmitted: onSubmit == null ? null : (_) => onSubmit!(),
      onEditingComplete: onSubmit == null ? null : () {},
      onChanged: onChanged,
      inputFormatters: [if (limit > 0) _Utf8Limit(limit)],
      style: style,
      decoration: InputDecoration(
        hintText: hint,
        hintStyle: style.copyWith(color: Palette.dim),
        isDense: true,
        filled: true,
        fillColor: Palette.bg,
        contentPadding: const EdgeInsets.symmetric(
          horizontal: 14,
          vertical: 10,
        ),
        enabledBorder: const OutlineInputBorder(
          borderSide: BorderSide(color: Palette.line2),
          borderRadius: BorderRadius.all(Radius.circular(6)),
        ),
        focusedBorder: const OutlineInputBorder(
          borderSide: BorderSide(color: Palette.amberDim),
          borderRadius: BorderRadius.all(Radius.circular(6)),
        ),
      ),
    );
  }
}

Future<bool> _confirm(
  BuildContext context,
  String title,
  String text,
  String yes,
) async {
  final ok = await showDialog<bool>(
    context: context,
    builder: (context) => _Dialog(
      title: title,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Text(text),
          const SizedBox(height: 20),
          Row(
            mainAxisAlignment: MainAxisAlignment.end,
            children: [
              _Button(
                label: 'Cancel',
                ghost: true,
                onTap: () => Navigator.of(context).pop(false),
              ),
              const SizedBox(width: 10),
              _Button(label: yes, onTap: () => Navigator.of(context).pop(true)),
            ],
          ),
        ],
      ),
    ),
  );

  return ok ?? false;
}

Color _topicColor(int rgb) => Color(0xff000000 | rgb);

Future<String?> _popup(
  BuildContext context,
  Offset at,
  List<PopupMenuEntry<String>> items,
) {
  return showMenu<String>(
    context: context,
    position: RelativeRect.fromLTRB(at.dx, at.dy, at.dx, at.dy),
    popUpAnimationStyle: AnimationStyle.noAnimation,
    items: items,
  );
}

PopupMenuItem<String> _item(String value, String label) {
  return PopupMenuItem<String>(
    value: value,
    height: 40,
    padding: EdgeInsets.zero,
    child: _MenuRow(label: label),
  );
}

class _MenuRow extends StatefulWidget {
  const _MenuRow({required this.label});

  final String label;

  @override
  State<_MenuRow> createState() => _MenuRowState();
}

class _MenuRowState extends State<_MenuRow> {
  bool _hover = false;

  @override
  Widget build(BuildContext context) {
    return MouseRegion(
      onEnter: (_) => setState(() => _hover = true),
      onExit: (_) => setState(() => _hover = false),
      child: Container(
        width: double.infinity,
        height: 40,
        alignment: Alignment.centerLeft,
        padding: const EdgeInsets.symmetric(horizontal: 14),
        color: _hover ? Palette.line2 : null,
        child: Text(
          widget.label,
          style: TextStyle(color: _hover ? Palette.fg : Palette.fg2),
        ),
      ),
    );
  }
}

Future<void> _topicMenu(
  BuildContext context,
  Chat chat,
  Topic topic,
  Offset at,
  Future<void> Function(Future<void> Function()) act,
) async {
  final desk = Desk.instance;

  if (!desk.owns(chat) || chat.closed) {
    return;
  }

  final what = await _popup(context, at, [
    _item('rename', 'Rename topic'),
    _item('remove', 'Remove topic'),
  ]);

  if (what == null || !context.mounted) {
    return;
  }

  if (what == 'rename') {
    await _newTopic(context, chat, act, topic: topic);
    return;
  }

  final sure = await _confirm(
    context,
    'Remove ${topic.name}?',
    'It goes away for everyone in this chat, with what was said in it.',
    'Remove',
  );

  if (sure) {
    await act(() => desk.removeTopic(chat, topic.id));
  }
}

class _RightClick extends StatelessWidget {
  const _RightClick({required this.onMenu, required this.child});

  final void Function(Offset) onMenu;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Listener(
      onPointerDown: (e) {
        if (e.buttons == kSecondaryMouseButton) {
          onMenu(e.position);
        }
      },
      child: child,
    );
  }
}

String _line(Message message) {
  final desk = Desk.instance;

  final who = desk.mine(message) ? 'You' : desk.name(message.author);
  final group = message.text.isEmpty ? 'a group' : message.text;

  return switch (message.system) {
    '' => message.summary,
    'ambry' => 'The keys have been rotated, bundle ${message.text}',
    'joined' => '$who joined',
    'created' => 'You created $group',
    'added' => '$who added you to $group',
    'renamed' => '$who renamed the group to $group',
    'ended' => '$who ended the group',
    'removed' => '$who removed you from the group',
    'parted' => 'You left the group',
    _ => '$who left',
  };
}

String _two(int n) => n.toString().padLeft(2, '0');

String _clock(int at) {
  final t = DateTime.fromMillisecondsSinceEpoch(at);
  return '${_two(t.hour)}:${_two(t.minute)}';
}

String _when(int at) {
  final t = DateTime.fromMillisecondsSinceEpoch(at);
  final now = DateTime.now();

  if (t.year == now.year && t.month == now.month && t.day == now.day) {
    return _clock(at);
  }

  return '${t.year}-${_two(t.month)}-${_two(t.day)}';
}

String _day(int at) {
  final t = DateTime.fromMillisecondsSinceEpoch(at);
  final now = DateTime.now();
  final today = DateTime(now.year, now.month, now.day);
  final then = DateTime(t.year, t.month, t.day);
  final days = today.difference(then).inDays;

  if (days == 0) {
    return 'Today';
  }

  if (days == 1) {
    return 'Yesterday';
  }

  const names = [
    'January',
    'February',
    'March',
    'April',
    'May',
    'June',
    'July',
    'August',
    'September',
    'October',
    'November',
    'December',
  ];

  if (t.year == now.year) {
    return '${t.day} ${names[t.month - 1]}';
  }

  return '${t.day} ${names[t.month - 1]} ${t.year}';
}

String _size(int size) {
  if (size < 1024) {
    return '$size B';
  }

  if (size < 1024 * 1024) {
    return '${(size / 1024).toStringAsFixed(1)} KB';
  }

  return '${(size / (1024 * 1024)).toStringAsFixed(1)} MB';
}

String _duration(int ms) {
  final s = ms ~/ 1000;
  return '${s ~/ 60}:${_two(s % 60)}';
}
