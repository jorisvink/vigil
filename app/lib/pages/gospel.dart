import 'dart:async';

import 'package:flutter/material.dart';

import '../app.dart';
import '../vigil.dart';
import '../theme.dart';
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
  bool _collapsed = true;

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

    Vigil.instance
        .flag(_expandedKey)
        .then((on) {
          if (mounted) {
            setState(() => _collapsed = !on);
          }
        })
        .catchError((Object _) {});
  }

  static const _expandedKey = 'gospelExpanded';

  void _collapse(bool on) {
    setState(() => _collapsed = on);
    Vigil.instance.flag(_expandedKey, !on).catchError((Object _) => !on);
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

      chats.sort(
        (a, b) => (b.last?.time ?? DateTime(0)).compareTo(
          a.last?.time ?? DateTime(0),
        ),
      );

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

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;

    return StreamBuilder<List<Tunnel>>(
      stream: vigil.tunnelUpdates,
      initialData: vigil.tunnels,
      builder: (context, tunnels) => StreamBuilder<Status>(
        stream: vigil.statuses,
        initialData: vigil.status,
        builder: (context, snapshot) {
          final status = snapshot.data!;
          final voice = tunnels.data!.where((t) => t.summary.voice).firstOrNull;

          return TabPage(
            index: 3,
            rubric: 'Gospel',
            title: 'Your flock',
            glow: status.call?.alive ?? false,
            actions: [
              if (voice != null) ...[
                GlyphBtn(
                  GlyphKind.edit,
                  label: 'Chat settings',
                  onTap: chatSettings,
                ),
                GlyphBtn(
                  GlyphKind.plus,
                  label: 'New chat',
                  onTap: () => newChat(status),
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
                  action: BtnPair(
                    first: Btn(
                      label: 'Scan QR',
                      glyph: GlyphKind.scan,
                      expand: true,
                      onTap: () => push(const ScanPage()),
                    ),
                    second: Btn(
                      label: 'Paste',
                      glyph: GlyphKind.paste,
                      kind: BtnKind.secondary,
                      expand: true,
                      onTap: () => push(const PastePage()),
                    ),
                  ),
                )
              else ...[
                _GospelPanel(
                  voice: voice,
                  status: status,
                  busy: _busy,
                  onGospel: _gospel,
                  collapsed: _collapsed,
                  onCollapse: _collapse,
                ),
                ..._chatList(status),
                ..._peers(status),
              ],
            ],
          );
        },
      ),
    );
  }

  List<Widget> _chatList(Status status) {
    final chats = _list(status, _chats);

    return [
      const GroupLabel('Chats'),
      if (_error != null) Notice(text: _error!, kind: NoticeKind.warn),
      if (!status.chats.hardware)
        const Notice(
          text:
              'This device has no hardware key agreement, messages '
              'that arrive while it is locked are protected by the '
              'device key only.',
        ),
      if (_chats != null && chats.isEmpty && _error == null)
        EmptyState(
          glyph: GlyphKind.chat,
          title: 'No chats yet',
          text:
              'Messages are end-to-end encrypted, held until your '
              'peer is around and stored encrypted on this device.',
          action: Btn(
            label: 'New chat',
            glyph: GlyphKind.plus,
            onTap: () => newChat(status),
          ),
        ),
      for (final chat in chats)
        ChatRow(key: ValueKey(chat.peer), chat: chat, status: status),
    ];
  }

  List<Widget> _peers(Status status) {
    final own = ownPeer();
    final online = (status.call?.peers ?? const <String>[])
        .where((p) => p != own)
        .length;

    return [
      GroupLabel(
        'Peers',
        trailing: Text('$online online', style: Styles.rubric),
      ),
      Btn(
        label: 'Show all',
        glyph: GlyphKind.call,
        kind: BtnKind.secondary,
        expand: true,
        onTap: () => sheet<void>(context, (context) => const _PeerSheet()),
      ),
    ];
  }

  List<ChatSummary> _list(Status status, List<ChatSummary>? chats) {
    final group = status.chats.group;
    final list = chats ?? const <ChatSummary>[];

    if (chats == null) {
      return list;
    }

    if (group == null) {
      return list;
    }

    if (list.any((c) => c.peer == group)) {
      return list;
    }

    return [
      ChatSummary({'peer': group, 'unread': 0}),
      ...list,
    ];
  }
}

class _GospelPanel extends StatelessWidget {
  const _GospelPanel({
    required this.voice,
    required this.status,
    required this.busy,
    required this.onGospel,
    required this.collapsed,
    required this.onCollapse,
  });

  final Tunnel voice;
  final Status status;
  final bool busy;
  final void Function(bool) onGospel;
  final bool collapsed;
  final void Function(bool) onCollapse;

  @override
  Widget build(BuildContext context) {
    final on = status.gospel;
    final calls = status.call;
    final starting = status.connecting == voice.id;
    final state = _state(on, starting, calls);
    final toggle = Toggle(
      on: on,
      pending: busy || starting,
      label: 'Gospel',
      onTap: busy || starting ? null : () => onGospel(!on),
    );
    final fold = _Fold(
      collapsed: collapsed,
      onTap: () => onCollapse(!collapsed),
    );

    final header = Row(
      children: [
        StateDot(state),
        const SizedBox(width: 10),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Rubric(text: voice.summary.instance),
              const SizedBox(height: 4),
              Text(
                _brief(on, starting, calls),
                style: Styles.detail,
                overflow: TextOverflow.ellipsis,
              ),
            ],
          ),
        ),
        const SizedBox(width: 12),
        toggle,
        const SizedBox(width: 4),
        fold,
      ],
    );
    final liturgies = status.liturgies;

    return Panel(
      accent: state == Liveness.online,
      padding: const EdgeInsets.fromLTRB(18, 12, 8, 12),
      onTap: () => push(TunnelPage(tunnel: voice)),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          header,
          if (!collapsed && !on) ...[
            const SizedBox(height: 10),
            Text(_detail(on, calls), style: Styles.detail),
          ],
          if (on && calls == null && !starting) ...[
            const SizedBox(height: 14),
            Padding(
              padding: const EdgeInsets.only(right: 10),
              child: Btn(
                label: 'Start',
                expand: true,
                busy: busy,
                onTap: busy ? null : () => onGospel(true),
              ),
            ),
          ],
          if (!collapsed && on && liturgies.isNotEmpty) ...[
            const SizedBox(height: 12),
            for (final l in liturgies) _LiturgyRow(liturgy: l),
          ],
        ],
      ),
    );
  }

  String _brief(bool on, bool starting, CallStatus? calls) {
    if (!on || starting || calls == null) {
      return _title(on, starting, calls);
    }

    return '↓ ${bytes(status.gospelRx)} · ↑ ${bytes(status.gospelTx)}';
  }

  Liveness _state(bool on, bool starting, CallStatus? calls) {
    if (starting) {
      return Liveness.pending;
    }

    if (!on) {
      return Liveness.off;
    }

    if (calls == null) {
      return Liveness.off;
    }

    if (!calls.alive) {
      return Liveness.pending;
    }

    return Liveness.online;
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

  String _detail(bool on, CallStatus? calls) {
    if (!on) {
      return 'Turn it on to make calls and send messages';
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

class _PeerSheet extends StatefulWidget {
  const _PeerSheet();

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
    final own = ownPeer();
    final calls = status.call?.peers ?? const <String>[];
    final needle = _filter.text.trim().toLowerCase();
    final peers = <String>{
      ...calls,
      ...?status.chat?.peers.where((p) => p.present).map((p) => p.id),
      ...vigil.names.keys.where((p) => !isGroup(p)),
    }.where((p) => p != own).toList();

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
            sheetTitle('Peers'),
            const SizedBox(height: 16),
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
      push(ChatPage(peer: peer));
    }

    return Panel(
      padding: const EdgeInsets.fromLTRB(16, 6, 8, 6),
      onTap: chat,
      child: Row(
        children: [
          chatDot(status, peer),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  vigil.name(peer),
                  style: Styles.cardTitle,
                  overflow: TextOverflow.ellipsis,
                ),
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

class _Fold extends StatelessWidget {
  const _Fold({required this.collapsed, required this.onTap});

  final bool collapsed;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Semantics(
      button: true,
      label: collapsed ? 'Show Gospel details' : 'Hide Gospel details',
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.all(8),
          child: RotatedBox(
            quarterTurns: collapsed ? 1 : 3,
            child: const Glyph(GlyphKind.chevron, size: 18, color: Palette.dim),
          ),
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
      padding: const EdgeInsets.only(right: 10),
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

    return Vigil.instance.name('g${l.group}');
  }
}
