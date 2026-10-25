import 'package:flutter/widgets.dart';

import '../app.dart';
import '../vigil.dart';
import '../theme.dart';
import '../widgets/buttons.dart';
import '../widgets/marks.dart';
import '../widgets/network_map.dart';
import '../widgets/ui.dart';

class StatusPage extends StatelessWidget {
  const StatusPage({super.key});

  @override
  Widget build(BuildContext context) {
    return StreamBuilder<Status>(
      stream: Vigil.instance.statuses,
      initialData: Vigil.instance.status,
      builder: (context, snapshot) {
        final status = snapshot.data!;
        final running = status.running && status.mode != 'gospel';

        return TabPage(
          index: 2,
          rubric: 'Status',
          title: _title(status, running),
          glow: running && status.online,
          children: running
              ? _running(status)
              : [
                  EmptyState(
                    glyph: GlyphKind.status,
                    title: 'No tunnel active',
                    text:
                        'Connect one of your tunnels to see its peers '
                        'and traffic here.',
                    action: Btn(
                      label: 'Your tunnels',
                      kind: BtnKind.secondary,
                      onTap: () => goTab(NavTab.tunnels),
                    ),
                  ),
                ],
        );
      },
    );
  }

  String _title(Status status, bool running) {
    if (!running) {
      return 'Idle';
    }

    if (status.online) {
      return 'Online';
    }

    return 'Negotiating';
  }

  List<Widget> _running(Status status) {
    final peers = status.peers
      ..sort((a, b) => int.parse(a.id, radix: 16) - int.parse(b.id, radix: 16));
    final online = peers.where((p) => p.online).length;
    final rx = peers.fold<int>(0, (sum, p) => sum + p.rxBytes);
    final tx = peers.fold<int>(0, (sum, p) => sum + p.txBytes);
    final rxPkts = peers.fold<int>(0, (sum, p) => sum + p.rxPkts);
    final txPkts = peers.fold<int>(0, (sum, p) => sum + p.txPkts);

    final rows = <(String, String)>[
      ('Instance', status.instance),
      ('Mode', status.kind),
      ('Address', status.address),
    ];

    if (status.cathedral != null) {
      rows.add(('Cathedral', status.cathedral!));
    }

    if (status.liturgyAlive != null) {
      rows.add(('Liturgy', status.liturgyAlive! ? 'alive' : 'waiting'));
    }

    return [
      if (status.lockdown)
        const Notice(kind: NoticeKind.warn, text: lockdownNotice),
      StatGrid(
        tiles: [
          StatTile(label: 'Uptime', value: duration(status.uptime)),
          StatTile(
            label: 'Peers',
            value: '$online / ${peers.length}',
            detail: 'online',
          ),
          StatTile(
            label: '↓ Received',
            value: bytes(rx),
            detail: '$rxPkts packets',
          ),
          StatTile(
            label: '↑ Sent',
            value: bytes(tx),
            detail: '$txPkts packets',
          ),
        ],
      ),
      const GroupLabel('Session', index: 1),
      KeyList(rows: rows),
      if (status.mode == 'liturgy') ...[
        const GroupLabel('Network', index: 2),
        Panel(child: NetworkMap(status: status)),
      ],
      GroupLabel('Peers', index: status.mode == 'liturgy' ? 3 : 2),
      if (peers.isEmpty)
        Notice(
          text: status.mode == 'liturgy'
              ? 'The cathedral has not told us about any other peers in '
                    'our liturgy group yet.'
              : 'There is no peer yet.',
        )
      else
        for (final p in peers) _PeerCard(peer: p, mode: status.kind),
      const SizedBox(height: 4),
      BtnPair(
        first: Btn(
          label: 'Log',
          glyph: GlyphKind.log,
          kind: BtnKind.secondary,
          expand: true,
          onTap: () => goTab(NavTab.log),
        ),
        second: Btn(
          label: 'Disconnect',
          expand: true,
          onTap: () => Vigil.instance.disconnect().catchError(
            (Object e) => notify('$e'),
          ),
        ),
      ),
    ];
  }
}

class _PeerCard extends StatelessWidget {
  const _PeerCard({required this.peer, required this.mode});

  final PeerStatus peer;
  final String mode;

  @override
  Widget build(BuildContext context) {
    final p = peer;
    final path = mode == 'tunnel'
        ? 'direct'
        : p.p2p
        ? 'peer-to-peer'
        : 'relayed';

    Widget pair(String key, String value) => Padding(
      padding: const EdgeInsets.only(top: 8),
      child: Row(
        children: [
          SizedBox(
            width: 96,
            child: Text(key.toUpperCase(), style: Styles.key),
          ),
          Expanded(
            child: Text(
              value,
              style: Styles.value,
              textAlign: TextAlign.right,
              overflow: TextOverflow.ellipsis,
            ),
          ),
        ],
      ),
    );

    return Panel(
      accent: p.online,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              StateDot(p.online ? Liveness.online : Liveness.pending),
              const SizedBox(width: 10),
              Expanded(
                child: Text(
                  p.tunnel == null
                      ? mode == 'liturgy'
                            ? p.id
                            : 'Peer'
                      : '${p.id} - ${p.tunnel}',
                  style: Styles.cardTitle,
                  overflow: TextOverflow.ellipsis,
                ),
              ),
              Tag(path, accent: p.online),
            ],
          ),
          const SizedBox(height: 6),
          pair('Endpoint', p.address),
          pair('Path MTU', '${p.mtu}'),
          pair('↓ / ↑', '${bytes(p.rxBytes)} / ${bytes(p.txBytes)}'),
          pair('SPI', '${p.txSpi} / ${p.rxSpi}'),
          if (p.ambry != '00000000') pair('Ambry', p.ambry),
          pair('Last heard', p.idle == 0 ? 'now' : '${p.idle}s ago'),
        ],
      ),
    );
  }
}
