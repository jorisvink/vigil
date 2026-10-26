import 'package:flutter/widgets.dart';

import '../app.dart';
import '../vigil.dart';
import '../theme.dart';
import '../widgets/background.dart';
import '../widgets/buttons.dart';
import '../widgets/marks.dart';
import '../widgets/ui.dart';
import 'import.dart';
import 'tunnel.dart';

class TunnelsPage extends StatefulWidget {
  const TunnelsPage({super.key});

  @override
  State<TunnelsPage> createState() => _TunnelsPageState();
}

class _TunnelsPageState extends State<TunnelsPage> {
  String? _last;
  bool _busy = false;

  Future<void> _toggle(Tunnel tunnel) async {
    setState(() => _busy = true);
    await toggle(tunnel);
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
          final list = tunnels.data!.where((t) => !t.summary.voice).toList();
          final alone = status.running && status.mode == 'gospel';
          final tunnel =
              alone || (voice != null && status.connecting == voice.id)
              ? Status(const {'running': false}, null, null, status.lockdown)
              : status;

          if (tunnel.running) {
            _last = tunnel.tunnelId;
          }

          return TabPage(
            index: 1,
            rubric: 'Tunnels',
            title: 'Your tunnels',
            glow: tunnel.online,
            actions: [
              GlyphBtn(
                GlyphKind.plus,
                label: 'Add a tunnel',
                onTap: () => push(const ScanPage()),
              ),
            ],
            children: [
              if (list.isNotEmpty || tunnel.running)
                _ConnectionPanel(
                  status: tunnel,
                  offer: _offer(list),
                  busy: _busy && tunnel.connecting == null,
                  onToggle: _toggle,
                ),
              if (status.running && !status.linked) const LinkNotice(),
              if (list.isNotEmpty && !status.background)
                const BackgroundNotice(),
              if (list.isEmpty)
                EmptyState(
                  glyph: GlyphKind.scan,
                  title: 'No tunnels yet',
                  text:
                      'Scan the QR code you were given, it holds '
                      'everything needed to set up a tunnel.',
                  action: _addButtons(first: true),
                )
              else ...[
                GroupLabel(
                  'Tunnels stored on this device',
                  trailing: Text('${list.length}', style: Styles.rubric),
                ),
                for (final t in list)
                  _TunnelCard(
                    tunnel: t,
                    state: liveness(status, t.id),
                    connecting: status.connecting == t.id,
                    onToggle: _busy || status.connecting != null
                        ? null
                        : () => _toggle(t),
                  ),
                const SizedBox(height: 4),
                _addButtons(),
              ],
            ],
          );
        },
      ),
    );
  }

  Tunnel? _offer(List<Tunnel> list) {
    if (list.isEmpty) {
      return null;
    }

    for (final t in list) {
      if (t.id == _last) {
        return t;
      }
    }

    return list.first;
  }

  Widget _addButtons({bool first = false}) {
    return BtnPair(
      first: Btn(
        label: 'Scan QR',
        glyph: GlyphKind.scan,
        kind: first ? BtnKind.primary : BtnKind.secondary,
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
    );
  }
}

class _ConnectionPanel extends StatelessWidget {
  const _ConnectionPanel({
    required this.status,
    required this.offer,
    required this.busy,
    required this.onToggle,
  });

  final Status status;
  final Tunnel? offer;
  final bool busy;
  final void Function(Tunnel) onToggle;

  @override
  Widget build(BuildContext context) {
    final connecting = status.connecting != null;

    final state = connecting
        ? Liveness.pending
        : !status.running
        ? Liveness.off
        : status.online
        ? Liveness.online
        : Liveness.pending;

    final title = connecting
        ? 'Connecting'
        : switch (state) {
            Liveness.off => 'Not connected',
            Liveness.pending => 'Negotiating',
            Liveness.online => 'Connected',
          };

    final id = connecting
        ? status.connecting
        : status.running
        ? status.tunnelId
        : null;
    final active = Vigil.instance.tunnels.where((t) => t.id == id).firstOrNull;
    final target = active ?? offer;

    return Panel(
      accent: state == Liveness.online,
      padding: EdgeInsets.zero,
      child: Stack(
        children: [
          Positioned.fill(
            child: NetBackdrop(
              intensity: switch (state) {
                Liveness.off => 0.45,
                Liveness.pending => 0.7,
                Liveness.online => 1.2,
              },
            ),
          ),
          Padding(
            padding: const EdgeInsets.fromLTRB(18, 18, 18, 18),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    StateDot(state),
                    const SizedBox(width: 10),
                    Rubric(text: status.running ? status.kind : 'Connection'),
                  ],
                ),
                const SizedBox(height: 14),
                Text(title, style: Styles.state),
                const SizedBox(height: 6),
                Text(
                  status.running && !connecting
                      ? '${status.instance} · ${status.address}'
                      : target == null
                      ? 'Add a tunnel to get started'
                      : '${target.summary.instance} · '
                            '${target.summary.endpoint}',
                  style: Styles.detail,
                  overflow: TextOverflow.ellipsis,
                ),
                if (status.running && !connecting) ...[
                  const SizedBox(height: 18),
                  _Figures(status: status),
                ],
                const SizedBox(height: 20),
                if (target != null) _toggleBtn(target, connecting),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

extension on _ConnectionPanel {
  Widget _toggleBtn(Tunnel target, bool connecting) {
    return Btn(
      label: connecting
          ? 'Connecting ${target.summary.instance}'
          : status.running
          ? 'Disconnect'
          : 'Connect ${target.summary.instance}',
      kind: status.running || connecting ? BtnKind.secondary : BtnKind.primary,
      expand: true,
      busy: busy,
      onTap: connecting ? null : () => onToggle(target),
    );
  }
}

class _Figures extends StatelessWidget {
  const _Figures({required this.status});

  final Status status;

  @override
  Widget build(BuildContext context) {
    final peers = status.peers;
    final rx = peers.fold<int>(0, (sum, p) => sum + p.rxBytes);
    final tx = peers.fold<int>(0, (sum, p) => sum + p.txBytes);

    Widget figure(String label, String value) => Expanded(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label.toUpperCase(), style: Styles.key),
          const SizedBox(height: 4),
          Text(value, style: Styles.value),
        ],
      ),
    );

    return Row(
      children: [
        figure('Uptime', duration(status.uptime)),
        figure('↓ In', bytes(rx)),
        figure('↑ Out', bytes(tx)),
      ],
    );
  }
}

class _TunnelCard extends StatelessWidget {
  const _TunnelCard({
    required this.tunnel,
    required this.state,
    required this.connecting,
    required this.onToggle,
  });

  final Tunnel tunnel;
  final Liveness state;
  final bool connecting;
  final VoidCallback? onToggle;

  @override
  Widget build(BuildContext context) {
    final s = tunnel.summary;

    return Panel(
      accent: state != Liveness.off,
      padding: const EdgeInsets.fromLTRB(16, 14, 6, 14),
      onTap: () => push(TunnelPage(tunnel: tunnel)),
      child: Row(
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Tag(modeTag(s), accent: state != Liveness.off),
                    if (s.shroud) ...[
                      const SizedBox(width: 6),
                      const Tag('Shroud'),
                    ],
                  ],
                ),
                const SizedBox(height: 10),
                Text(
                  s.instance,
                  style: Styles.cardTitle,
                  overflow: TextOverflow.ellipsis,
                ),
                const SizedBox(height: 3),
                Text(
                  switch (state) {
                    Liveness.off => s.endpoint,
                    Liveness.pending =>
                      connecting ? 'Connecting…' : 'Negotiating keys…',
                    Liveness.online => 'Online · ${s.address}',
                  },
                  style: Styles.detail.copyWith(
                    color: state == Liveness.online ? Palette.good : null,
                  ),
                  overflow: TextOverflow.ellipsis,
                ),
              ],
            ),
          ),
          Toggle(
            on: state != Liveness.off,
            pending: state == Liveness.pending,
            onTap: onToggle,
          ),
        ],
      ),
    );
  }
}

String modeTag(Summary summary) => switch (summary.mode) {
  'cathedral' => 'Cathedral',
  'liturgy' => summary.tier6 ? 'Tier6' : 'Liturgy',
  'gospel' => 'Gospel',
  _ => 'Direct',
};
