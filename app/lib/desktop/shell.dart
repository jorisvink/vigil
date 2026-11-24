import 'dart:async';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../links.dart';
import '../reliquary.dart';
import '../theme.dart';
import '../voice.dart';
import '../widgets/marks.dart';
import '../vigil.dart' show flockOf;
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

class _Shell extends StatefulWidget {
  const _Shell();

  @override
  State<_Shell> createState() => _ShellState();
}

class _ShellState extends State<_Shell> {
  bool _favorites = false;
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

    Widget main = const _Empty();

    if (_favorites) {
      main = _Favorites(
        onOpen: (chat, topic) => _select(chat, topic),
        onClose: () => setState(() => _favorites = false),
      );
    } else if (chat != null) {
      main = _Conversation(
        key: ValueKey('${chat.id}/${desk.openTopic}'),
        chat: chat,
        topic: desk.openTopic,
        onTopic: (topic) => _select(chat, topic),
        onProblem: _say,
      );
    }

    return Row(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        _Sidebar(
          open: _favorites ? null : open,
          topic: desk.openTopic,
          onSelect: _select,
          onFavorites: () => setState(() => _favorites = true),
          onProblem: _say,
          act: _act,
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

class _Sidebar extends StatelessWidget {
  const _Sidebar({
    required this.open,
    required this.topic,
    required this.onSelect,
    required this.onFavorites,
    required this.onProblem,
    required this.act,
  });

  final String? open;
  final String topic;
  final void Function(Chat, String) onSelect;
  final VoidCallback onFavorites;
  final void Function(String) onProblem;
  final Future<void> Function(Future<void> Function()) act;

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final chats = desk.sorted;
    final rows = <Widget>[];

    for (final chat in chats) {
      rows.add(
        _ChatItem(
          chat: chat,
          active: chat.id == open && topic.isEmpty,
          onTap: () => onSelect(chat, ''),
          act: act,
        ),
      );

      final topics = chat.topics.values
          .where((t) => t.name.isNotEmpty)
          .toList();

      if (topics.isEmpty) {
        continue;
      }

      if (chat.compact) {
        rows.add(
          Padding(
            padding: const EdgeInsets.fromLTRB(68, 0, 16, 8),
            child: Wrap(
              spacing: 6,
              runSpacing: 6,
              children: [
                for (final t in topics)
                  _TopicTag(
                    chat: chat,
                    topic: t,
                    active: chat.id == open && topic == t.id,
                    onTap: () => onSelect(chat, t.id),
                    act: act,
                  ),
              ],
            ),
          ),
        );
        continue;
      }

      for (final (i, t) in topics.indexed) {
        rows.add(
          _TopicItem(
            chat: chat,
            topic: t,
            end: i == topics.length - 1,
            active: chat.id == open && topic == t.id,
            onTap: () => onSelect(chat, t.id),
            act: act,
          ),
        );
      }
    }

    return ColoredBox(
      color: Palette.bg2,
      child: SizedBox(
        width: 300,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            SizedBox(
              height: 56,
              child: Row(
                children: [
                  const SizedBox(width: Metrics.edge + 2),
                  const WordmarkSquare(),
                  const SizedBox(width: 12),
                  Text('VIGIL', style: Styles.wordmark),
                  const Spacer(),
                  _Icon(
                    icon: GlyphKind.plus,
                    tip: 'New chat',
                    onTap: () => _newChat(context, onSelect, act),
                  ),
                  _Icon(
                    icon: GlyphKind.star,
                    tip: 'Favorites',
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
            const ColoredBox(color: Palette.line, child: SizedBox(height: 1)),
            _PhoneState(),
            const ColoredBox(color: Palette.line, child: SizedBox(height: 1)),
            Expanded(
              child: chats.isEmpty
                  ? Center(child: Text('NO CHATS YET', style: Styles.rubric))
                  : ListView(
                      padding: const EdgeInsets.symmetric(vertical: 8),
                      children: rows,
                    ),
            ),
            const ColoredBox(color: Palette.line, child: SizedBox(height: 1)),
            _Me(),
          ],
        ),
      ),
    );
  }
}

class _Me extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;

    return Padding(
      padding: const EdgeInsets.symmetric(
        horizontal: Metrics.edge,
        vertical: 10,
      ),
      child: Row(
        children: [
          _Avatar(peer: '00', size: 30),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  desk.name('00'),
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    fontSize: 13.5,
                    fontWeight: FontWeight.w600,
                    color: Palette.fg,
                  ),
                ),
                Text(
                  desk.selves.isEmpty
                      ? 'Not heard from your phone'
                      : (desk.selves.values.toList()..sort()).join(' · '),
                  style: Styles.detail,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                ),
              ],
            ),
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

    return Padding(
      padding: const EdgeInsets.symmetric(
        horizontal: Metrics.edge + 2,
        vertical: 12,
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              _Dot(color: color),
              const SizedBox(width: 10),
              Expanded(
                child: Text(
                  text,
                  style: Styles.rubric.copyWith(color: color),
                  overflow: TextOverflow.ellipsis,
                ),
              ),
              if (desk.online && desk.queued > 0)
                Text('${desk.queued} WAITING', style: Styles.rubric),
            ],
          ),
          if (busy)
            const Padding(
              padding: EdgeInsets.only(top: 8),
              child: LinearProgressIndicator(
                minHeight: 2,
                color: Palette.amber,
                backgroundColor: Palette.amberDim,
              ),
            ),
        ],
      ),
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

class _ChatItem extends StatefulWidget {
  const _ChatItem({
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
  State<_ChatItem> createState() => _ChatItemState();
}

class _ChatItemState extends State<_ChatItem> {
  bool _hover = false;

  Future<void> _menu(Offset at) async {
    final desk = Desk.instance;
    final chat = widget.chat;
    final owned = chat.isGroup && desk.owns(chat);

    final what = await _popup(context, at, [
      _item('star', chat.starred ? 'Unstar' : 'Star'),
      if (chat.silenced)
        _item('unmute', 'Unmute')
      else ...[
        _item('mute1', 'Mute for an hour'),
        _item('mute8', 'Mute for 8 hours'),
        _item('mute', 'Mute'),
      ],
      if (chat.topics.isNotEmpty)
        _item(
          'compact',
          chat.compact ? 'Show topics as a tree' : 'Show topics as tags',
        ),
      if (desk.owns(chat) && !chat.closed) _item('topic', 'New topic'),
      if (owned) _item('edit', 'Edit group'),
      if (chat.isGroup && !chat.closed)
        _item(
          'leave',
          chat.owner == desk.selfIn(chat.id) ? 'End group' : 'Leave group',
        ),
      _item('delete', 'Delete chat'),
    ]);

    if (!mounted || what == null) {
      return;
    }

    final now = DateTime.now().millisecondsSinceEpoch;

    switch (what) {
      case 'star':
        await widget.act(() => desk.star(chat, !chat.starred));
      case 'unmute':
        await widget.act(() => desk.mute(chat, 0));
      case 'mute1':
        await widget.act(() => desk.mute(chat, now + 3600 * 1000));
      case 'mute8':
        await widget.act(() => desk.mute(chat, now + 8 * 3600 * 1000));
      case 'mute':
        await widget.act(() => desk.mute(chat, -1));
      case 'compact':
        await widget.act(() => desk.compact(chat, !chat.compact));
      case 'topic':
        await _newTopic(context, chat, widget.act);
      case 'edit':
        await showDialog<void>(
          context: context,
          builder: (_) => _GroupDialog(chat: chat, act: widget.act),
        );
      case 'leave':
        final sure = await _confirm(
          context,
          chat.owner == desk.selfIn(chat.id)
              ? 'End this group?'
              : 'Leave this group?',
          'Your phone does this for you, the messages here stay.',
          chat.owner == desk.selfIn(chat.id) ? 'End' : 'Leave',
        );
        if (sure) {
          await widget.act(() => desk.leaveGroup(chat));
        }
      case 'delete':
        final sure = await _confirm(
          context,
          'Delete this chat?',
          'Your phone deletes it too.',
          'Delete',
        );
        if (sure) {
          await widget.act(() => desk.delete(chat));
        }
    }
  }

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final chat = widget.chat;
    final main = chat.messages.where((m) => m.topic.isEmpty);
    final last = main.isEmpty ? null : main.last;
    final typing = desk.typers(chat, '');

    var preview = 'No messages';
    if (typing.isNotEmpty) {
      preview = chat.isGroup
          ? '${desk.name(typing.first)} is typing…'
          : 'typing…';
    } else if (last != null) {
      preview = _line(last);
      if (last.system.isEmpty) {
        if (desk.mine(last)) {
          preview = 'You: $preview';
        } else if (chat.isGroup) {
          preview = '${desk.name(last.author)}: $preview';
        }
      }
    }

    final bg = widget.active
        ? Palette.amberWash
        : _hover
        ? Palette.bg3
        : const Color(0x00000000);

    return MouseRegion(
      cursor: SystemMouseCursors.click,
      onEnter: (_) => setState(() => _hover = true),
      onExit: (_) => setState(() => _hover = false),
      child: _RightClick(
        onMenu: _menu,
        child: GestureDetector(
          onTap: widget.onTap,
          child: Container(
            margin: const EdgeInsets.symmetric(horizontal: 8, vertical: 1),
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 9),
            color: bg,
            child: Row(
              children: [
                _Avatar(
                  peer: chat.isGroup ? '' : chat.id,
                  label: desk.title(chat),
                  group: chat.isGroup,
                  size: 36,
                  presence: !chat.isGroup,
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(
                        children: [
                          Expanded(
                            child: Text(
                              desk.title(chat),
                              overflow: TextOverflow.ellipsis,
                              style: TextStyle(
                                fontSize: 14,
                                fontWeight: chat.unreadAll > 0
                                    ? FontWeight.w600
                                    : FontWeight.w500,
                                color: chat.closed ? Palette.dim : Palette.fg,
                              ),
                            ),
                          ),
                          if (chat.starred)
                            const Padding(
                              padding: EdgeInsets.only(left: 6),
                              child: Glyph(
                                GlyphKind.star,
                                size: 12,
                                color: Palette.amber,
                              ),
                            ),
                          if (chat.silenced)
                            const Padding(
                              padding: EdgeInsets.only(left: 6),
                              child: Glyph(
                                GlyphKind.snoozed,
                                size: 12,
                                color: Palette.dim,
                              ),
                            ),
                          if (last != null) ...[
                            const SizedBox(width: 6),
                            Text(_when(last.at), style: Styles.detail),
                          ],
                        ],
                      ),
                      const SizedBox(height: 2),
                      Row(
                        children: [
                          Expanded(
                            child: Text(
                              preview,
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style: TextStyle(
                                fontSize: 13,
                                height: 1.3,
                                fontStyle: typing.isNotEmpty
                                    ? FontStyle.italic
                                    : FontStyle.normal,
                                color: typing.isNotEmpty
                                    ? Palette.amber
                                    : chat.unread > 0
                                    ? Palette.fg2
                                    : Palette.dim,
                              ),
                            ),
                          ),
                          if (chat.unread > 0)
                            _Unread(count: chat.unread, quiet: chat.silenced),
                        ],
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _TopicItem extends StatefulWidget {
  const _TopicItem({
    required this.chat,
    required this.topic,
    required this.end,
    required this.active,
    required this.onTap,
    required this.act,
  });

  final Chat chat;
  final Topic topic;
  final bool end;
  final bool active;
  final VoidCallback onTap;
  final Future<void> Function(Future<void> Function()) act;

  @override
  State<_TopicItem> createState() => _TopicItemState();
}

class _TopicItemState extends State<_TopicItem> {
  bool _hover = false;

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final chat = widget.chat;
    final topic = widget.topic;
    final unread = topic.unread;
    final typing = desk.typers(chat, topic.id);
    final said = chat.messages.where((m) => m.topic == topic.id);

    var preview = '';
    if (typing.isNotEmpty) {
      preview = chat.isGroup
          ? '${desk.name(typing.first)} is typing…'
          : 'typing…';
    } else if (said.isNotEmpty) {
      preview = _line(said.last);
    }

    final bg = widget.active
        ? Palette.amberWash
        : _hover
        ? Palette.bg3
        : const Color(0x00000000);

    return IntrinsicHeight(
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          SizedBox(
            width: 52,
            child: CustomPaint(painter: _Branch(end: widget.end)),
          ),
          Expanded(
            child: MouseRegion(
              cursor: SystemMouseCursors.click,
              onEnter: (_) => setState(() => _hover = true),
              onExit: (_) => setState(() => _hover = false),
              child: _RightClick(
                onMenu: (at) =>
                    _topicMenu(context, chat, topic, at, widget.act),
                child: GestureDetector(
                  onTap: widget.onTap,
                  child: Container(
                    margin: const EdgeInsets.only(right: 8, bottom: 1),
                    padding: const EdgeInsets.fromLTRB(8, 6, 12, 6),
                    decoration: BoxDecoration(
                      color: bg,
                      borderRadius: const BorderRadius.all(Radius.circular(8)),
                    ),
                    child: Row(
                      children: [
                        _Dot(color: _topicColor(topic.color), size: 8),
                        const SizedBox(width: 9),
                        ConstrainedBox(
                          constraints: const BoxConstraints(maxWidth: 110),
                          child: Text(
                            topic.name,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: TextStyle(
                              fontSize: 13,
                              fontWeight: unread > 0
                                  ? FontWeight.w600
                                  : FontWeight.w500,
                              color: unread > 0 || widget.active
                                  ? Palette.fg
                                  : Palette.fg2,
                            ),
                          ),
                        ),
                        const SizedBox(width: 8),
                        Expanded(
                          child: Text(
                            preview,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: TextStyle(
                              fontSize: 12.5,
                              fontStyle: typing.isNotEmpty
                                  ? FontStyle.italic
                                  : FontStyle.normal,
                              color: typing.isNotEmpty
                                  ? Palette.amber
                                  : unread > 0
                                  ? Palette.fg2
                                  : Palette.dim,
                            ),
                          ),
                        ),
                        if (unread > 0)
                          _Unread(count: unread, quiet: chat.silenced),
                      ],
                    ),
                  ),
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _TopicTag extends StatefulWidget {
  const _TopicTag({
    required this.chat,
    required this.topic,
    required this.active,
    required this.onTap,
    required this.act,
  });

  final Chat chat;
  final Topic topic;
  final bool active;
  final VoidCallback onTap;
  final Future<void> Function(Future<void> Function()) act;

  @override
  State<_TopicTag> createState() => _TopicTagState();
}

class _TopicTagState extends State<_TopicTag> {
  bool _hover = false;

  @override
  Widget build(BuildContext context) {
    final desk = Desk.instance;
    final chat = widget.chat;
    final topic = widget.topic;
    final unread = topic.unread;
    final typing = desk.typers(chat, topic.id).isNotEmpty;
    final lit = _topicColor(topic.color);

    var color = Color.lerp(lit, Palette.bg2, 0.82)!;
    var ink = Color.lerp(lit, Palette.dim, 0.55)!;

    if (unread > 0) {
      color = lit;
      ink = lit.computeLuminance() > 0.35 ? Palette.bg : Palette.fg;
    }

    if (_hover || widget.active) {
      color = Color.lerp(color, Palette.fg, 0.08)!;
    }

    return MouseRegion(
      cursor: SystemMouseCursors.click,
      onEnter: (_) => setState(() => _hover = true),
      onExit: (_) => setState(() => _hover = false),
      child: _RightClick(
        onMenu: (at) => _topicMenu(context, chat, topic, at, widget.act),
        child: GestureDetector(
          onTap: widget.onTap,
          child: Container(
            padding: const EdgeInsets.fromLTRB(9, 4, 9, 4),
            decoration: BoxDecoration(
              color: color,
              border: widget.active
                  ? Border.all(color: Palette.amberDim)
                  : null,
              borderRadius: const BorderRadius.all(Radius.circular(11)),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                ConstrainedBox(
                  constraints: const BoxConstraints(maxWidth: 140),
                  child: Text(
                    typing ? '${topic.name} …' : topic.name,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(
                      fontSize: 12,
                      height: 1.2,
                      fontWeight: FontWeight.w600,
                      color: ink,
                    ),
                  ),
                ),
                if (unread > 0) ...[
                  const SizedBox(width: 6),
                  Container(
                    padding: const EdgeInsets.symmetric(
                      horizontal: 5,
                      vertical: 1,
                    ),
                    decoration: const BoxDecoration(
                      color: Palette.bg,
                      borderRadius: BorderRadius.all(Radius.circular(8)),
                    ),
                    child: Text(
                      '$unread',
                      style: const TextStyle(
                        fontSize: 10.5,
                        height: 1.2,
                        color: Palette.amber,
                      ),
                    ),
                  ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _Branch extends CustomPainter {
  const _Branch({required this.end});

  final bool end;

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = Palette.line2
      ..strokeWidth = 1.5
      ..style = PaintingStyle.stroke;
    const x = 38.0;
    final y = size.height / 2;
    final path = Path()
      ..moveTo(x, 0)
      ..lineTo(x, y - 8)
      ..quadraticBezierTo(x, y, x + 8, y)
      ..lineTo(size.width, y);

    canvas.drawPath(path, paint);

    if (!end) {
      canvas.drawLine(Offset(x, y - 8), Offset(x, size.height), paint);
    }
  }

  @override
  bool shouldRepaint(_Branch old) => old.end != end;
}

class _Unread extends StatelessWidget {
  const _Unread({required this.count, this.quiet = false});

  final int count;
  final bool quiet;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.only(left: 8),
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
      decoration: BoxDecoration(
        color: quiet ? Palette.line2 : Palette.amber,
        borderRadius: const BorderRadius.all(Radius.circular(8)),
      ),
      child: Text(
        '$count',
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
                                    '${desk.name(message.author)} · '
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

class _SettingsDialog extends StatefulWidget {
  const _SettingsDialog({required this.act});

  final Future<void> Function(Future<void> Function()) act;

  @override
  State<_SettingsDialog> createState() => _SettingsDialogState();
}

class _SettingsDialogState extends State<_SettingsDialog> {
  final _nick = TextEditingController(text: Desk.instance.selfNick);

  @override
  void dispose() {
    _nick.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    final nick = _nick.text.trim();

    Navigator.of(context).pop();

    if (nick != Desk.instance.selfNick) {
      await widget.act(() => Desk.instance.nick(nick));
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
                onTap: () => widget.act(() async {
                  await desk.avatar();
                }),
              ),
              const SizedBox(width: 10),
              if (desk.selfAvatar.isNotEmpty)
                _Button(
                  label: 'Remove',
                  ghost: true,
                  onTap: () => widget.act(desk.dropAvatar),
                ),
            ],
          ),
          const SizedBox(height: 20),
          Text('NICKNAME', style: Styles.rubric),
          const SizedBox(height: 6),
          _Field(
            controller: _nick,
            hint: 'What your peers see',
            limit: nickMax,
            onSubmit: _save,
          ),
          for (final signed in desk.selves.keys.map(desk.selfSigned))
            if (signed.isNotEmpty) ...[
            const SizedBox(height: 6),
            Text(
              'Your peers see your signed name $signed.',
              style: Styles.detail,
            ),
          ],
          const SizedBox(height: 8),
          const Text(
            'Your phone keeps these, they change there too.',
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
              const SizedBox(width: 10),
              _Button(label: 'Save', onTap: _save),
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
    final text = (label ?? desk.name(peer)).replaceFirst('~', '');
    final words = text.trim().split(RegExp(r'\s+'));
    var initials = words.first.isEmpty ? '?' : words.first[0];
    if (words.length > 1 && words.last.isNotEmpty) {
      initials += words.last[0];
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

    final state = desk.peers[peer];
    Color? dot;

    if (state != null && state.online) {
      dot = Palette.good;
    } else if (state != null && state.present) {
      dot = Palette.amber;
    }

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
      style: TextStyle(
        fontSize: mono ? 12.5 : 14.5,
        fontFamily: mono ? Fonts.mono : null,
        color: Palette.fg,
      ),
      decoration: InputDecoration(
        hintText: hint,
        hintStyle: const TextStyle(color: Palette.dim),
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
