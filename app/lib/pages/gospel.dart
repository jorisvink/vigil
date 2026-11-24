import 'dart:async';

import 'package:flutter/material.dart';

import '../app.dart';
import '../vigil.dart';
import '../theme.dart';
import '../widgets/background.dart';
import '../widgets/buttons.dart';
import '../widgets/marks.dart';
import '../widgets/ui.dart';
import 'call.dart';
import 'chats.dart';
import 'import.dart';
import 'tunnel.dart';

class GospelPage extends StatefulWidget {
  const GospelPage({super.key});

  @override
  State<GospelPage> createState() => _GospelPageState();
}

final invitesWanted = ValueNotifier<int>(0);

class _GospelPageState extends State<GospelPage> with WidgetsBindingObserver {
  StreamSubscription<Status>? _updates;
  List<ChatSummary>? _chats;
  String? _error;
  int _version = -1;
  bool _busy = false;
  bool _unreadOnly = false;
  bool _peersOpen = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);

    _updates = Vigil.instance.statuses.listen((status) {
      if (status.chats.version != _version) {
        _load();
      }
    });

    invitesWanted.addListener(_showPeers);
    _load();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    invitesWanted.removeListener(_showPeers);
    _updates?.cancel();
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      _load();
    }
  }

  Future<void> _load() async {
    final state = WidgetsBinding.instance.lifecycleState;

    if (state != null && state != AppLifecycleState.resumed) {
      return;
    }

    _version = Vigil.instance.status.chats.version;

    try {
      final chats = await Vigil.instance.chats();

      if (mounted) {
        setState(() {
          _chats = chats;
          _error = null;
        });
      }
    } on VigilError catch (e) {
      if (mounted) {
        setState(() => _error = e.message);
      }
    }
  }

  Future<void> _gospel(bool on) async {
    setState(() => _busy = true);

    try {
      await Vigil.instance.gospel(on);
    } on VigilError catch (e) {
      notify(e.message);
    }

    if (mounted) {
      setState(() => _busy = false);
    }
  }

  Future<void> _showPeers() async {
    final chats = [
      for (final c in _chats ?? const <ChatSummary>[])
        if (!isGroup(c.peer) && !c.request) c.peer,
    ];
    final invites = [
      for (final c in _chats ?? const <ChatSummary>[])
        if (c.request) c.peer,
    ]..sort();
    if (_peersOpen) {
      return;
    }

    _peersOpen = true;

    final pick = await push<String>(
      _PeersPage(chats: chats, invites: invites),
    );

    _peersOpen = false;


    if (pick == _PeersPage.group) {
      await newGroup(knownPeers(Vigil.instance.status, chats));
    } else if (pick == _PeersPage.kek) {
      await chatById();
    }
  }

  void _showGospel(List<Tunnel> voices) {
    sheet<void>(context, (context) => _GospelSheet(voices: voices));
  }

  void _showDesktop() {
    sheet<void>(context, (context) => const _DesktopSheet());
  }

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;

    return StreamBuilder<List<Tunnel>>(
      stream: vigil.tunnelUpdates,
      initialData: vigil.tunnels,
      builder: (context, tunnels) => LiveStatus(
        builder: (context, status) {
          final voices = gospels(tunnels.data!);
          final voice = voices.firstOrNull;

          return TabPage(
            index: 3,
            rubric: 'Gospel',
            heading: false,
            glow: status.call?.alive ?? false,
            fill: voice != null && _quiet(status) ? const _Quiet() : null,
            actions: [
              if (voice != null) ...[
                _StatusBtn(
                  state: gospelStates(voices, status),
                  onTap: () => _showGospel(voices),
                ),
                if (status.chat?.companion case final desktop?)
                  _DesktopBtn(
                    state: _desktopState(desktop).$1,
                    onTap: _showDesktop,
                  ),
                GlyphBtn(
                  GlyphKind.edit,
                  label: 'Chat settings',
                  onTap: chatSettings,
                ),
                _PeersBtn(
                  invites: (_chats ?? const <ChatSummary>[]).any(
                    (c) => c.request,
                  ),
                  onTap: _showPeers,
                ),
              ],
            ],
            children: [
              if (voice == null)
                EmptyState(
                  glyph: GlyphKind.chat,
                  title: 'No Gospel configuration yet',
                  text:
                      'Scan the QR code of a Gospel configuration to make '
                      'calls and send text messages to anyone in your flock.',
                  action: Btn(
                    label: 'Add config',
                    glyph: GlyphKind.scan,
                    expand: true,
                    onTap: addConfig,
                  ),
                )
              else ...[
                ?_offline(voices, status),
                if (status.running && !status.linked) const LinkNotice(),
                if (!status.background) const BackgroundNotice(),
                ..._chatList(status),
              ],
            ],
          );
        },
      ),
    );
  }

  // Only shown when Gospel cannot carry chats at all, everything else
  // lives behind the status dot in the top bar.
  Widget? _offline(List<Tunnel> voices, Status status) {
    final starting = voices.any((v) => status.connecting == v.id);

    if (starting || (status.gospel && status.call != null)) {
      return null;
    }

    return _Offline(
      text: status.gospel ? 'Gospel is not running' : 'Gospel is off',
      action: status.gospel ? 'Start' : 'Turn on',
      busy: _busy,
      onAction: () => _gospel(true),
      onTap: () => _showGospel(voices),
    );
  }

  bool _quiet(Status status) {
    final all = _list(status, _chats).where((c) => !c.request).toList();

    return _unreadOnly && all.isNotEmpty && !all.any(_unread);
  }

  List<Widget> _chatList(Status status) {
    final all = _list(status, _chats).where((c) => !c.request).toList();
    final chats = all.where((c) => !_unreadOnly || _unread(c)).toList()
      ..sort(_order);
    final reorder = !_unreadOnly && chats.length > 1;

    return [
      if (_error != null) Notice(text: _error!, kind: NoticeKind.warn),
      if (!status.chats.hardware)
        const Notice(
          text:
              'This device has no hardware key agreement, messages '
              'that arrive while it is locked are protected by the '
              'device key only.',
        ),
      if (_chats != null && all.isEmpty && _error == null)
        EmptyState(
          glyph: GlyphKind.chat,
          title: 'No chats yet',
          text:
              'Messages are end-to-end encrypted, held until your '
              'peer is around and stored encrypted on this device.',
          action: Btn(
            label: 'New chat',
            glyph: GlyphKind.plus,
            onTap: _showPeers,
          ),
        ),
      if (all.isNotEmpty)
        GroupLabel(
          'Chats',
          trailing: _UnreadSort(
            on: _unreadOnly,
            onTap: () => setState(() => _unreadOnly = !_unreadOnly),
          ),
        ),
      if (chats.isNotEmpty)
        Gapped(
          child: ReorderableListView(
            shrinkWrap: true,
            padding: EdgeInsets.zero,
            physics: const NeverScrollableScrollPhysics(),
            buildDefaultDragHandles: false,
            proxyDecorator: (child, index, animation) => child,
            onReorderItem: (from, to) => _move(chats, from, to),
            children: [
              for (final (i, chat) in chats.indexed)
                Padding(
                  key: ValueKey(chat.peer),
                  padding: EdgeInsets.only(
                    bottom: i == chats.length - 1 ? 0 : 2,
                  ),
                  child: ChatRow(
                    chat: chat,
                    status: status,
                    handle: reorder ? i : null,
                  ),
                ),
            ],
          ),
        ),
    ];
  }

  // Favorites always stay on top, a drag is clamped to the block the
  // chat belongs to (favorites or the rest).
  Future<void> _move(List<ChatSummary> chats, int from, int to) async {
    final chat = chats.removeAt(from);
    final stars = chats.where((c) => c.star).length;

    if (chat.star) {
      to = to.clamp(0, stars);
    } else {
      to = to.clamp(stars, chats.length);
    }

    chats.insert(to, chat);

    setState(() {
      for (final (i, c) in chats.indexed) {
        c.json['order'] = i;
      }
    });

    try {
      await Vigil.instance.orderChats([for (final c in chats) c.peer]);
    } on VigilError catch (e) {
      notify(e.message);
    }
  }

  int _order(ChatSummary a, ChatSummary b) {
    if (a.star != b.star) {
      return a.star ? -1 : 1;
    }

    if (a.order != b.order) {
      if (a.order == -1) {
        return 1;
      }

      if (b.order == -1) {
        return -1;
      }

      return a.order.compareTo(b.order);
    }

    final vigil = Vigil.instance;
    final name = vigil.name(a.peer).toLowerCase();
    final order = name.compareTo(vigil.name(b.peer).toLowerCase());

    if (order != 0) {
      return order;
    }

    return a.peer.compareTo(b.peer);
  }

  static bool _unread(ChatSummary chat) =>
      chat.unread > 0 || chat.topics.any((t) => t.unread > 0);

  List<ChatSummary> _list(Status status, List<ChatSummary>? chats) =>
      chats ?? const <ChatSummary>[];
}

class _UnreadSort extends StatelessWidget {
  const _UnreadSort({required this.on, required this.onTap});

  final bool on;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Pressable(
      onTap: onTap,
      label: on ? 'Show all chats' : 'Show only unread chats',
      builder: (context, pressed) {
        final tint = on || pressed ? Palette.amber : Palette.fg2;

        return Padding(
          padding: const EdgeInsets.symmetric(vertical: 6),
          child: Container(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 7),
            decoration: BoxDecoration(
              color: on ? Palette.amberWash : null,
              border: Border.all(color: on ? Palette.amberDim : Palette.line2),
              borderRadius: BorderRadius.circular(16),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                Glyph(GlyphKind.sort, size: 13, color: tint),
                const SizedBox(width: 6),
                Text(
                  'Unread only',
                  style: Styles.detail.copyWith(
                    fontSize: 12.5,
                    height: 1,
                    color: tint,
                  ),
                ),
              ],
            ),
          ),
        );
      },
    );
  }
}

List<Tunnel> gospels(List<Tunnel> tunnels) =>
    tunnels.where((t) => t.summary.voice).toList()
      ..sort((a, b) => a.summary.flockId.compareTo(b.summary.flockId));

Liveness gospelState(Tunnel voice, Status status) {
  final calls = status.call;

  if (status.connecting == voice.id) {
    return Liveness.pending;
  }

  if (!status.gospel || calls == null) {
    return Liveness.off;
  }

  if (!calls.gospelAlive(voice.summary.flockId)) {
    return Liveness.pending;
  }

  return Liveness.online;
}

Liveness gospelStates(List<Tunnel> voices, Status status) {
  final states = [for (final v in voices) gospelState(v, status)];

  if (states.contains(Liveness.pending)) {
    return Liveness.pending;
  }

  if (states.contains(Liveness.online)) {
    return Liveness.online;
  }

  return Liveness.off;
}

class _StatusBtn extends StatelessWidget {
  const _StatusBtn({required this.state, required this.onTap});

  final Liveness state;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Pressable(
      onTap: onTap,
      label: 'Gospel status',
      builder: (context, pressed) => SizedBox(
        width: 44,
        height: 44,
        child: Center(
          child: Container(
            padding: const EdgeInsets.all(7),
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              border: Border.all(
                color: pressed ? Palette.amber : Palette.line2,
              ),
            ),
            child: StateDot(state),
          ),
        ),
      ),
    );
  }
}

class _Offline extends StatelessWidget {
  const _Offline({
    required this.text,
    required this.action,
    required this.busy,
    required this.onAction,
    required this.onTap,
  });

  final String text;
  final String action;
  final bool busy;
  final VoidCallback onAction;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Panel(
      padding: const EdgeInsets.fromLTRB(16, 8, 8, 8),
      onTap: onTap,
      child: Row(
        children: [
          const StateDot(Liveness.off),
          const SizedBox(width: 10),
          Expanded(child: Text(text, style: Styles.detail)),
          Btn(
            label: action,
            kind: BtnKind.secondary,
            busy: busy,
            onTap: busy ? null : onAction,
          ),
        ],
      ),
    );
  }
}

(Liveness, String, String?) _desktopState(Map<String, dynamic> desktop) {
  final online = desktop['online'] as bool? ?? false;
  final queued = desktop['queued'] as int? ?? 0;
  final version = desktop['version'] as int? ?? 0;
  final other = desktop['other'] as int?;
  final title = online ? 'Linked' : 'Away';
  final waiting = queued > 0 ? '$queued waiting' : null;

  if (online && other != null && other < version) {
    return (
      Liveness.pending,
      title,
      'Your desktop runs an older Vigil, update it to sync',
    );
  }

  if (online && other != null && other > version) {
    return (
      Liveness.pending,
      title,
      'Update Vigil on this phone to sync with your desktop',
    );
  }

  if (!online) {
    return (Liveness.pending, title, waiting);
  }

  return (Liveness.online, title, waiting);
}

class _DesktopBtn extends StatelessWidget {
  const _DesktopBtn({required this.state, required this.onTap});

  final Liveness state;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Pressable(
      onTap: onTap,
      label: 'Desktop status',
      builder: (context, pressed) => SizedBox(
        width: 44,
        height: 44,
        child: Center(
          child: Stack(
            clipBehavior: Clip.none,
            children: [
              Glyph(
                GlyphKind.desktop,
                color: pressed ? Palette.amber : Palette.fg2,
              ),
              Positioned(right: -2, top: -1, child: StateDot(state, size: 6)),
            ],
          ),
        ),
      ),
    );
  }
}

class _DesktopSheet extends StatelessWidget {
  const _DesktopSheet();

  Future<void> _unlink(BuildContext context) async {
    if (!await confirm(
      title: 'Unlink desktop',
      text:
          'Unlink your desktop from this phone. All synchronization is '
          'stopped and your desktop state remains as-is.',
      action: 'Unlink',
    )) {
      return;
    }

    try {
      await Vigil.instance.unlinkDesktop();
    } on VigilError catch (e) {
      notify(e.message);
      return;
    }

    if (context.mounted) {
      Navigator.of(context).pop();
    }
  }

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;

    return StreamBuilder<Status>(
      stream: vigil.statuses,
      initialData: vigil.status,
      builder: (context, snapshot) {
        final desktop = snapshot.data!.chat?.companion;

        if (desktop == null) {
          return const SizedBox.shrink();
        }

        final (state, title, detail) = _desktopState(desktop);

        return Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            sheetTitle('Desktop'),
            const SizedBox(height: 16),
            Row(
              children: [
                StateDot(state),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(title, style: Styles.value),
                      if (detail != null) ...[
                        const SizedBox(height: 2),
                        Text(detail, style: Styles.detail),
                      ],
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 16),
            Btn(
              label: 'Unlink',
              kind: BtnKind.secondary,
              expand: true,
              onTap: () => _unlink(context),
            ),
          ],
        );
      },
    );
  }
}

class _GospelSheet extends StatefulWidget {
  const _GospelSheet({required this.voices});

  final List<Tunnel> voices;

  @override
  State<_GospelSheet> createState() => _GospelSheetState();
}

class _GospelSheetState extends State<_GospelSheet> {
  bool _busy = false;

  Future<void> _gospel(bool on) async {
    setState(() => _busy = true);

    try {
      await Vigil.instance.gospel(on);
    } on VigilError catch (e) {
      notify(e.message);
    }

    if (mounted) {
      setState(() => _busy = false);
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
        final voices = gospels(vigil.tunnels);
        final on = status.gospel;
        final calls = status.call;
        final starting = voices.any((v) => status.connecting == v.id);

        return Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            sheetTitle('Gospel'),
            const SizedBox(height: 16),
            Row(
              children: [
                StateDot(gospelStates(voices, status)),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(_title(on, starting, calls), style: Styles.value),
                      const SizedBox(height: 2),
                      Text(
                        _detail(voices, on, calls),
                        style: Styles.detail,
                        overflow: TextOverflow.ellipsis,
                      ),
                    ],
                  ),
                ),
                const SizedBox(width: 12),
                Toggle(
                  on: on,
                  pending: _busy || starting,
                  label: 'Gospel',
                  onTap: _busy || starting ? null : () => _gospel(!on),
                ),
              ],
            ),
            if (on && calls != null) ...[
              const SizedBox(height: 10),
              Text(
                '↓ ${bytes(status.gospelRx)} · ↑ ${bytes(status.gospelTx)}',
                style: Styles.detail,
              ),
            ],
            if (on && calls == null && !starting) ...[
              const SizedBox(height: 14),
              Btn(
                label: 'Start',
                expand: true,
                busy: _busy,
                onTap: _busy ? null : () => _gospel(true),
              ),
            ],
            const SizedBox(height: 16),
            Flexible(
              child: ListView(
                shrinkWrap: true,
                padding: EdgeInsets.zero,
                children: [
                  for (final voice in voices)
                    _GospelRow(voice: voice, status: status),
                ],
              ),
            ),
            const SizedBox(height: 8),
            Btn(
              label: 'Add Gospel',
              glyph: GlyphKind.scan,
              kind: BtnKind.secondary,
              expand: true,
              onTap: () {
                Navigator.of(context).pop();
                addConfig();
              },
            ),
          ],
        );
      },
    );
  }

  String _title(bool on, bool starting, CallStatus? calls) {
    if (!on) {
      return 'Off';
    }

    if (starting) {
      return 'Starting';
    }

    if (calls == null) {
      return 'Not running';
    }

    if (!calls.alive) {
      return 'Waiting for the cathedral';
    }

    return 'Ready for calls and messages';
  }

  String _detail(List<Tunnel> voices, bool on, CallStatus? calls) {
    if (!on) {
      return 'No calls or messages';
    }

    if (calls == null) {
      if (voices.length == 1) {
        return voices.first.summary.endpoint;
      }

      return '${voices.length} flocks';
    }

    if (calls.peers.length == 1) {
      return '1 peer online';
    }

    return '${calls.peers.length} peers online';
  }
}

class _GospelRow extends StatelessWidget {
  const _GospelRow({required this.voice, required this.status});

  final Tunnel voice;
  final Status status;

  @override
  Widget build(BuildContext context) {
    final flock = voice.summary.flockId;
    final online = (status.call?.peers ?? const <String>[])
        .where((p) => flockOf(p) == flock)
        .length;
    final liturgies = status.gospel ? status.liturgies(flock) : const [];

    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Panel(
        onTap: () {
          Navigator.of(context).pop();
          push(TunnelPage(tunnel: voice));
        },
        padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                StateDot(gospelState(voice, status)),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(voice.summary.instance, style: Styles.cardTitle),
                      Text(
                        online == 1
                            ? 'Flock $flock · 1 peer online'
                            : 'Flock $flock · $online peers online',
                        style: Styles.detail,
                      ),
                    ],
                  ),
                ),
                const Glyph(GlyphKind.chevron, size: 16, color: Palette.dim),
              ],
            ),
            if (liturgies.isNotEmpty) ...[
              const SizedBox(height: 8),
              for (final l in liturgies) _LiturgyRow(liturgy: l),
            ],
          ],
        ),
      ),
    );
  }
}

List<String> knownPeers(Status status, List<String> chats) {
  final vigil = Vigil.instance;

  return <String>{
    ...?status.call?.peers,
    ...?status.chat?.peers.where((p) => p.present).map((p) => p.id),
    ...vigil.names.keys.where((p) => !isGroup(p)),
    ...chats,
  }.where((p) => isPeer(p) && !vigil.mine(p)).toList()..sort();
}

class _PeersPage extends StatefulWidget {
  const _PeersPage({required this.chats, required this.invites});

  static const group = 'group';
  static const kek = 'kek';

  final List<String> chats;
  final List<String> invites;

  @override
  State<_PeersPage> createState() => _PeersPageState();
}

class _PeersPageState extends State<_PeersPage> {
  final _filter = TextEditingController();
  final _answered = <String>{};
  StreamSubscription<Status>? _updates;
  late List<String> _invites = widget.invites;
  int _version = -1;
  bool? _reachable;
  String? _busy;

  Future<void> _reach(bool on) async {
    try {
      final reachable = await Vigil.instance.reachable(on);

      if (mounted) {
        setState(() => _reachable = reachable);
      }
    } on VigilError catch (e) {
      notify(e.message);
    }
  }

  Future<void> _reload() async {
    try {
      final chats = await Vigil.instance.chats();

      if (mounted) {
        setState(() {
          _invites = [
            for (final c in chats)
              if (c.request) c.peer,
          ]..sort();
          _answered.retainAll(_invites);
        });
      }
    } on VigilError catch (_) {
      return;
    }
  }

  Future<void> _answer(String peer, String action) async {
    setState(() => _busy = peer);

    try {
      if (action == 'ignore') {
        await Vigil.instance.ignoreChat(peer, true);
      } else {
        await Vigil.instance.acceptChat(peer, action == 'accept');
      }

      _answered.add(peer);
    } on VigilError catch (e) {
      notify(e.message);
    }

    if (mounted) {
      setState(() => _busy = null);
    }
  }

  @override
  void initState() {
    super.initState();
    _filter.addListener(() => setState(() {}));

    _updates = Vigil.instance.statuses.listen((status) {
      if (status.chats.version != _version) {
        _version = status.chats.version;
        _reload();
      }
    });

    unawaited(
      Vigil.instance
          .reachable()
          .then((on) {
            if (mounted) {
              setState(() => _reachable = on);
            }
          })
          .catchError((Object _) {}),
    );
  }

  @override
  void dispose() {
    _updates?.cancel();
    _filter.dispose();
    super.dispose();
  }

  List<String> _flocks(List<String> peers) =>
      {for (final p in peers) flockOf(p)}.toList()..sort();

  String _flockName(String flock) {
    final voices = gospels(Vigil.instance.tunnels);
    final voice = voices.where((v) => v.summary.flockId == flock).firstOrNull;

    if (voice == null) {
      return 'Flock $flock';
    }

    return '${voice.summary.instance} · $flock';
  }

  List<String> _peers(Status status) {
    final vigil = Vigil.instance;
    final calls = status.call?.peers ?? const <String>[];
    final needle = _filter.text.trim().toLowerCase();
    final peers = knownPeers(status, widget.chats);

    peers.retainWhere(
      (p) => p.contains(needle) || vigil.name(p).toLowerCase().contains(needle),
    );

    peers.sort((a, b) {
      final online = (calls.contains(a) ? 0 : 1) - (calls.contains(b) ? 0 : 1);

      if (online != 0) {
        return online;
      }

      return a.compareTo(b);
    });

    return peers;
  }

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;

    return StreamBuilder<Status>(
      stream: vigil.statuses,
      initialData: vigil.status,
      builder: (context, snapshot) {
        final status = snapshot.data!;
        final calls = status.call?.peers ?? const <String>[];
        final peers = _peers(status);

        final waiting = [
          for (final id in _invites)
            if (!_answered.contains(id)) id,
        ];

        return PushedPage(
          rubric: '${calls.length} online',
          title: 'Peers',
          children: [
            if (_reachable != null)
              Padding(
                padding: const EdgeInsets.only(bottom: 8),
                child: Panel(
                  padding: const EdgeInsets.fromLTRB(16, 10, 8, 10),
                  child: Row(
                    children: [
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text(
                              'Reachable by anyone',
                              style: Styles.cardTitle,
                            ),
                            Text(
                              _reachable!
                                  ? 'Anyone in your flock can message and '
                                        'call you'
                                  : 'Peers you did not accept show up as '
                                        'invites',
                              style: Styles.detail,
                            ),
                          ],
                        ),
                      ),
                      Toggle(
                        on: _reachable!,
                        label: 'Reachable by anyone',
                        onTap: () => _reach(!_reachable!),
                      ),
                    ],
                  ),
                ),
              ),
            if (waiting.isNotEmpty) ...[
              const GroupLabel('Invites'),
              const SizedBox(height: 10),
              for (final id in waiting)
                _Invite(
                  peer: id,
                  busy: _busy == id,
                  onAnswer: (action) => _answer(id, action),
                ),
              const SizedBox(height: 8),
            ],
            _Action(
              title: 'New group',
              text: 'Pick who will join the group, you become the owner',
              onTap: () => Navigator.of(context).pop(_PeersPage.group),
            ),
            _Action(
              title: 'Chat by KEK ID',
              text: 'Anyone in your flock, messages wait until they are online',
              onTap: () => Navigator.of(context).pop(_PeersPage.kek),
            ),
            _Action(
              title: 'Ignored peers',
              text: 'All peers you have ignored.',
              glyph: GlyphKind.chevron,
              onTap: () => sheet<void>(
                context,
                (context) => const _IgnoredSheet(),
              ),
            ),
            const SizedBox(height: 4),
            Pre(
              child: TextField(
                controller: _filter,
                textInputAction: TextInputAction.search,
                style: Styles.pre,
                cursorColor: Palette.amber,
                decoration: InputDecoration.collapsed(
                  hintText: 'Name or peer id',
                  hintStyle: Styles.pre.copyWith(color: Palette.dim),
                ),
              ),
            ),
            const SizedBox(height: 12),
            if (peers.isEmpty)
              const Padding(
                padding: EdgeInsets.symmetric(vertical: 8),
                child: Text(
                  'No peer matches, peers show up here once they are '
                  'around or once you named them.',
                  style: Styles.body,
                ),
              ),
            for (final flock in _flocks(peers)) ...[
              if (gospels(vigil.tunnels).length > 1) ...[
                GroupLabel(_flockName(flock)),
                const SizedBox(height: 10),
              ],
              for (final id in peers.where((p) => flockOf(p) == flock))
                Padding(
                  padding: const EdgeInsets.only(bottom: 8),
                  child: _PeerRow(
                    peer: id,
                    status: status,
                    callable: calls.contains(id) && status.phone == null,
                  ),
                ),
            ],
          ],
        );
      },
    );
  }
}

class _PeerRow extends StatelessWidget {
  const _PeerRow({
    required this.peer,
    required this.status,
    required this.callable,
  });

  final String peer;
  final Status status;
  final bool callable;

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;

    void chat() {
      Navigator.of(context).pop();
      push(ChatPage(peer: peer), opaque: false);
    }

    return Panel(
      padding: const EdgeInsets.fromLTRB(16, 6, 8, 6),
      onTap: chat,
      child: Row(
        children: [
          chatPresence(
            status,
            peer,
            Avatar(peer, margin: EdgeInsets.zero),
            ring: Palette.bg2,
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                PeerName(peer, style: Styles.cardTitle, shrink: 9),
                if (vigil.name(peer) != peer)
                  FittedBox(
                    fit: BoxFit.scaleDown,
                    child: Text(peer, style: Styles.detail),
                  ),
              ],
            ),
          ),
          if (videoCalls)
            GlyphBtn(
              GlyphKind.video,
              label: 'Video call ${vigil.name(peer)}',
              onTap: callable
                  ? () {
                      Navigator.of(context).pop();
                      callPeer(peer, video: true);
                    }
                  : null,
            ),
          GlyphBtn(
            GlyphKind.call,
            label: 'Call ${vigil.name(peer)}',
            onTap: callable
                ? () {
                    Navigator.of(context).pop();
                    callPeer(peer);
                  }
                : null,
          ),
          GlyphBtn(
            GlyphKind.chat,
            label: 'Chat with ${vigil.name(peer)}',
            onTap: chat,
          ),
        ],
      ),
    );
  }
}

class _Action extends StatelessWidget {
  const _Action({
    required this.title,
    required this.text,
    required this.onTap,
    this.glyph = GlyphKind.plus,
  });

  final String title;
  final String text;
  final VoidCallback onTap;
  final GlyphKind glyph;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Panel(
        padding: const EdgeInsets.fromLTRB(16, 12, 12, 12),
        onTap: onTap,
        child: Row(
          children: [
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(title, style: Styles.cardTitle),
                  Text(text, style: Styles.detail),
                ],
              ),
            ),
            Glyph(glyph, color: Palette.amber),
          ],
        ),
      ),
    );
  }
}

class _PeersBtn extends StatelessWidget {
  const _PeersBtn({required this.invites, required this.onTap});

  final bool invites;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Pressable(
      onTap: onTap,
      label: invites ? 'Peers, invites waiting' : 'Peers',
      builder: (context, pressed) => SizedBox(
        width: 44,
        height: 44,
        child: Center(
          child: Stack(
            clipBehavior: Clip.none,
            children: [
              Glyph(
                GlyphKind.person,
                color: pressed ? Palette.amber : Palette.fg2,
              ),
              if (invites)
                const Positioned(
                  right: -2,
                  top: -1,
                  child: StateDot(Liveness.pending, size: 6),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class _Invite extends StatelessWidget {
  const _Invite({
    required this.peer,
    required this.busy,
    required this.onAnswer,
  });

  final String peer;
  final bool busy;
  final ValueChanged<String> onAnswer;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Panel(
        accent: true,
        padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Avatar(peer),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      PeerName(peer, style: Styles.cardTitle),
                      if (Vigil.instance.name(peer) != peer)
                        Text(peer, style: Styles.detail),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 10),
            Row(
              children: [
                Expanded(
                  child: Btn(
                    label: 'ACCEPT',
                    kind: BtnKind.good,
                    expand: true,
                    busy: busy,
                    onTap: () => onAnswer('accept'),
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: Btn(
                    label: 'DECLINE',
                    kind: BtnKind.bad,
                    expand: true,
                    busy: busy,
                    onTap: () => onAnswer('decline'),
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: Btn(
                    label: 'IGNORE',
                    kind: BtnKind.secondary,
                    expand: true,
                    busy: busy,
                    onTap: () => onAnswer('ignore'),
                  ),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _IgnoredSheet extends StatefulWidget {
  const _IgnoredSheet();

  @override
  State<_IgnoredSheet> createState() => _IgnoredSheetState();
}

class _IgnoredSheetState extends State<_IgnoredSheet> {
  List<String>? _peers;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    try {
      final peers = await Vigil.instance.ignored();

      if (mounted) {
        setState(() => _peers = peers);
      }
    } on VigilError catch (e) {
      notify(e.message);
    }
  }

  Future<void> _remove(String peer) async {
    try {
      await Vigil.instance.ignoreChat(peer, false);
    } on VigilError catch (e) {
      notify(e.message);
    }

    await _load();
  }

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;
    final peers = _peers;

    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        sheetTitle('Ignored peers'),
        const SizedBox(height: 16),
        if (peers != null && peers.isEmpty)
          const Padding(
            padding: EdgeInsets.symmetric(vertical: 8),
            child: Text(
              'You are not ignoring anyone.',
              style: Styles.body,
            ),
          ),
        if (peers != null && peers.isNotEmpty)
          ConstrainedBox(
            constraints: BoxConstraints(
              maxHeight: MediaQuery.sizeOf(context).height * 0.5,
            ),
            child: ListView(
              shrinkWrap: true,
              padding: EdgeInsets.zero,
              children: [
                for (final id in peers)
                  Padding(
                    padding: const EdgeInsets.only(bottom: 8),
                    child: Panel(
                      padding: const EdgeInsets.fromLTRB(16, 6, 8, 6),
                      child: Row(
                        children: [
                          Avatar(id),
                          Expanded(
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                PeerName(
                                  id,
                                  style: Styles.cardTitle,
                                  shrink: 9,
                                ),
                                if (vigil.name(id) != id)
                                  Text(id, style: Styles.detail),
                              ],
                            ),
                          ),
                          Btn(
                            label: 'Remove',
                            kind: BtnKind.ghost,
                            onTap: () => _remove(id),
                          ),
                        ],
                      ),
                    ),
                  ),
              ],
            ),
          ),
      ],
    );
  }
}

class _LiturgyRow extends StatelessWidget {
  const _LiturgyRow({required this.liturgy});

  final LiturgyStatus liturgy;

  @override
  Widget build(BuildContext context) {
    final l = liturgy;

    return Padding(
      padding: EdgeInsets.zero,
      child: Column(
        children: [
          const Line(),
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 9),
            child: Row(
              children: [
                StateDot(l.alive ? Liveness.online : Liveness.pending, size: 6),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(_label(l), style: Styles.value),
                      const SizedBox(height: 2),
                      Text(
                        '${l.cathedral ?? 'no cathedral'} · ${l.group}',
                        style: Styles.detail.copyWith(fontSize: 11),
                        overflow: TextOverflow.ellipsis,
                      ),
                    ],
                  ),
                ),
                const SizedBox(width: 10),
                Column(
                  crossAxisAlignment: CrossAxisAlignment.end,
                  children: [
                    Text(
                      '↓ ${bytes(l.rx)}',
                      style: Styles.detail.copyWith(fontSize: 11),
                    ),
                    Text(
                      '↑ ${bytes(l.tx)}',
                      style: Styles.detail.copyWith(fontSize: 11),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  String _label(LiturgyStatus l) {
    switch (l.kind) {
      case 'signaling':
        return 'Call signaling';
      case 'discovery':
        return 'Call discovery';
      case 'chat':
        return 'Chat signaling';
    }

    return 'Liturgy ${l.group}';
  }
}

class _Quiet extends StatelessWidget {
  const _Quiet();

  @override
  Widget build(BuildContext context) {
    return const Padding(
      padding: EdgeInsets.symmetric(vertical: 32),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Priest(),
          SizedBox(height: 18),
          SolidCross(),
          SizedBox(height: 18),
          Text(
            'In silentio et spe erit fortitudo vestra.',
            style: Styles.detail,
            textAlign: TextAlign.center,
          ),
        ],
      ),
    );
  }
}
