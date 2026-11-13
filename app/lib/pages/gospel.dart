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

class _GospelPageState extends State<GospelPage> with WidgetsBindingObserver {
  StreamSubscription<Status>? _updates;
  List<ChatSummary>? _chats;
  String? _error;
  int _version = -1;
  bool _busy = false;
  bool _unreadOnly = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);

    _updates = Vigil.instance.statuses.listen((status) {
      if (status.chats.version != _version) {
        _load();
      }
    });

    _load();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
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
        if (!isGroup(c.peer)) c.peer,
    ];
    final pick = await sheet<String>(
      context,
      (context) => _PeerSheet(chats: chats),
    );

    if (pick == _PeerSheet.group) {
      await newGroup(knownPeers(Vigil.instance.status, chats));
    } else if (pick == _PeerSheet.kek) {
      await chatById();
    }
  }

  void _showGospel(Tunnel voice) {
    sheet<void>(context, (context) => _GospelSheet(voice: voice));
  }

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;

    return StreamBuilder<List<Tunnel>>(
      stream: vigil.tunnelUpdates,
      initialData: vigil.tunnels,
      builder: (context, tunnels) => LiveStatus(
        builder: (context, status) {
          final voice = tunnels.data!.where((t) => t.summary.voice).firstOrNull;

          return TabPage(
            index: 3,
            rubric: 'Gospel',
            heading: false,
            glow: status.call?.alive ?? false,
            fill: voice != null && _quiet(status) ? const _Quiet() : null,
            actions: [
              if (voice != null) ...[
                _StatusBtn(
                  state: gospelState(voice, status),
                  onTap: () => _showGospel(voice),
                ),
                GlyphBtn(
                  GlyphKind.edit,
                  label: 'Chat settings',
                  onTap: chatSettings,
                ),
                GlyphBtn(GlyphKind.person, label: 'Peers', onTap: _showPeers),
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
                ?_offline(voice, status),
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
  Widget? _offline(Tunnel voice, Status status) {
    final starting = status.connecting == voice.id;

    if (starting || (status.gospel && status.call != null)) {
      return null;
    }

    return _Offline(
      text: status.gospel ? 'Gospel is not running' : 'Gospel is off',
      action: status.gospel ? 'Start' : 'Turn on',
      busy: _busy,
      onAction: () => _gospel(true),
      onTap: () => _showGospel(voice),
    );
  }

  bool _quiet(Status status) {
    final all = _list(status, _chats);

    return _unreadOnly && all.isNotEmpty && !all.any(_unread);
  }

  List<Widget> _chatList(Status status) {
    final all = _list(status, _chats);
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
                    bottom: i == chats.length - 1 ? 0 : Metrics.gap,
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
        final tint = on || pressed ? Palette.amber : Palette.dim;

        return Padding(
          padding: const EdgeInsets.symmetric(vertical: 6),
          child: Container(
            padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 4),
            decoration: BoxDecoration(
              color: on ? Palette.amberWash : null,
              border: Border.all(color: on ? Palette.amberDim : Palette.line2),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                Glyph(GlyphKind.sort, size: 12, color: tint),
                const SizedBox(width: 5),
                Text(
                  'UNREAD',
                  style: Styles.rubric.copyWith(
                    fontSize: 9.5,
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

Liveness gospelState(Tunnel voice, Status status) {
  final calls = status.call;

  if (status.connecting == voice.id) {
    return Liveness.pending;
  }

  if (!status.gospel || calls == null) {
    return Liveness.off;
  }

  if (!calls.alive) {
    return Liveness.pending;
  }

  return Liveness.online;
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
            padding: const EdgeInsets.all(5),
            decoration: BoxDecoration(
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

class _GospelSheet extends StatefulWidget {
  const _GospelSheet({required this.voice});

  final Tunnel voice;

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
    final voice = widget.voice;

    return StreamBuilder<Status>(
      stream: vigil.statuses,
      initialData: vigil.status,
      builder: (context, snapshot) {
        final status = snapshot.data!;
        final on = status.gospel;
        final calls = status.call;
        final starting = status.connecting == voice.id;

        return Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            sheetTitle(voice.summary.instance),
            const SizedBox(height: 16),
            Row(
              children: [
                StateDot(gospelState(voice, status)),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(_title(on, starting, calls), style: Styles.value),
                      const SizedBox(height: 2),
                      Text(
                        _detail(voice, on, calls),
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
            if (on && status.liturgies.isNotEmpty) ...[
              const SizedBox(height: 12),
              for (final l in status.liturgies) _LiturgyRow(liturgy: l),
            ],
            const SizedBox(height: 16),
            Btn(
              label: 'Tunnel details',
              kind: BtnKind.secondary,
              expand: true,
              onTap: () {
                Navigator.of(context).pop();
                push(TunnelPage(tunnel: voice));
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

  String _detail(Tunnel voice, bool on, CallStatus? calls) {
    if (!on) {
      return 'No calls or messages';
    }

    if (calls == null) {
      return voice.summary.endpoint;
    }

    if (calls.peers.length == 1) {
      return '1 peer online';
    }

    return '${calls.peers.length} peers online';
  }
}

List<String> knownPeers(Status status, List<String> chats) {
  final vigil = Vigil.instance;
  final own = ownPeer();

  return <String>{
    ...?status.call?.peers,
    ...?status.chat?.peers.where((p) => p.present).map((p) => p.id),
    ...vigil.names.keys.where((p) => !isGroup(p)),
    ...chats,
  }.where((p) => p != own).toList()..sort();
}

class _PeerSheet extends StatefulWidget {
  const _PeerSheet({required this.chats});

  static const group = 'group';
  static const kek = 'kek';

  final List<String> chats;

  @override
  State<_PeerSheet> createState() => _PeerSheetState();
}

class _PeerSheetState extends State<_PeerSheet> {
  final _filter = TextEditingController();

  @override
  void initState() {
    super.initState();
    _filter.addListener(() => setState(() {}));
  }

  @override
  void dispose() {
    _filter.dispose();
    super.dispose();
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

        return Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Expanded(child: sheetTitle('Peers')),
                Text('${calls.length} online', style: Styles.rubric),
              ],
            ),
            const SizedBox(height: 16),
            _Action(
              title: 'New group',
              text: 'Pick the members, you own the group',
              onTap: () => Navigator.of(context).pop(_PeerSheet.group),
            ),
            _Action(
              title: 'Chat by KEK ID',
              text: 'Anyone in your flock, messages wait until they are online',
              onTap: () => Navigator.of(context).pop(_PeerSheet.kek),
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
            ConstrainedBox(
              constraints: BoxConstraints(
                maxHeight: MediaQuery.sizeOf(context).height * 0.5,
              ),
              child: peers.isEmpty
                  ? const Padding(
                      padding: EdgeInsets.symmetric(vertical: 8),
                      child: Text(
                        'No peer matches, peers show up here once they are '
                        'around or once you named them.',
                        style: Styles.body,
                      ),
                    )
                  : ListView(
                      shrinkWrap: true,
                      padding: EdgeInsets.zero,
                      children: [
                        for (final id in peers)
                          Padding(
                            padding: const EdgeInsets.only(bottom: 8),
                            child: _PeerRow(
                              peer: id,
                              status: status,
                              callable:
                                  calls.contains(id) && status.phone == null,
                            ),
                          ),
                      ],
                    ),
            ),
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
          chatDot(status, peer),
          const SizedBox(width: 12),
          Avatar(peer),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                PeerName(peer, style: Styles.cardTitle),
                if (vigil.name(peer) != 'Peer $peer')
                  Text('Peer $peer', style: Styles.detail),
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
  const _Action({required this.title, required this.text, required this.onTap});

  final String title;
  final String text;
  final VoidCallback onTap;

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
            const Glyph(GlyphKind.plus, color: Palette.amber),
          ],
        ),
      ),
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
