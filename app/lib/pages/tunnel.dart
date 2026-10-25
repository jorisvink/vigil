import 'package:flutter/widgets.dart';

import '../app.dart';
import '../vigil.dart';
import '../theme.dart';
import '../widgets/buttons.dart';
import '../widgets/ui.dart';
import 'edit.dart';
import 'tunnels.dart';

class TunnelPage extends StatefulWidget {
  const TunnelPage({super.key, required this.tunnel});

  final Tunnel tunnel;

  @override
  State<TunnelPage> createState() => _TunnelPageState();
}

class _TunnelPageState extends State<TunnelPage> {
  bool _busy = false;

  Future<void> _run(Future<void> Function() action) async {
    setState(() => _busy = true);

    try {
      await action();
    } on VigilError catch (e) {
      notify(e.message);
    } finally {
      if (mounted) {
        setState(() => _busy = false);
      }
    }
  }

  Future<void> _remove() async {
    final ok = await confirm(
      title: 'Remove ${widget.tunnel.summary.instance}?',
      text:
          'The keys for this tunnel are wiped from this device, you '
          'will need the QR code again to restore it.',
      action: 'Remove',
    );

    if (!ok) {
      return;
    }

    await _run(() async {
      final status = Vigil.instance.status;
      if (status.running && status.tunnelId == widget.tunnel.id) {
        await Vigil.instance.disconnect();
      }

      await Vigil.instance.remove(widget.tunnel.id);
      navigatorKey.currentState?.maybePop();
      notify(
        widget.tunnel.summary.voice
            ? 'Gospel configuration removed.'
            : 'Tunnel removed.',
        warn: false,
      );
    });
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
        builder: (context, snapshot) => _build(
          tunnels.data!.where((t) => t.id == widget.tunnel.id).firstOrNull ??
              widget.tunnel,
          snapshot.data!,
        ),
      ),
    );
  }

  Widget _build(Tunnel tunnel, Status status) {
    final summary = tunnel.summary;
    final calls = status.call;
    final state = summary.voice
        ? calls == null
              ? Liveness.off
              : calls.alive
              ? Liveness.online
              : Liveness.pending
        : liveness(status, tunnel.id);

    return PushedPage(
      rubric: summary.modeName,
      title: summary.instance,
      bottom: summary.voice
          ? null
          : Btn(
              label: status.connecting == tunnel.id
                  ? 'Connecting'
                  : state == Liveness.off
                  ? 'Connect'
                  : 'Disconnect',
              kind: state == Liveness.off ? BtnKind.primary : BtnKind.secondary,
              expand: true,
              busy: _busy && status.connecting == null,
              onTap: status.connecting != null
                  ? null
                  : () => _run(() => toggle(tunnel)),
            ),
      children: [
        Panel(
          accent: state == Liveness.online,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  StateDot(state),
                  const SizedBox(width: 10),
                  Text(
                    summary.voice
                        ? switch (state) {
                            Liveness.off => 'NOT RUNNING',
                            Liveness.pending => 'WAITING FOR THE CATHEDRAL',
                            Liveness.online => 'READY FOR CALLS',
                          }
                        : switch (state) {
                            Liveness.off => 'NOT CONNECTED',
                            Liveness.pending =>
                              status.connecting == tunnel.id
                                  ? 'CONNECTING'
                                  : 'NEGOTIATING KEYS',
                            Liveness.online =>
                              'ONLINE · ${duration(status.uptime)}',
                          },
                    style: Styles.rubric.copyWith(
                      color: state == Liveness.online
                          ? Palette.good
                          : Palette.dim,
                    ),
                  ),
                  const Spacer(),
                  Tag(modeTag(summary), accent: true),
                ],
              ),
              const SizedBox(height: 14),
              Text(_explain(summary), style: Styles.body),
            ],
          ),
        ),
        if (status.lockdown && status.tunnelId == tunnel.id)
          const Notice(kind: NoticeKind.warn, text: lockdownNotice),
        const GroupLabel('Settings', index: 1),
        KeyList(rows: summary.rows),
        Btn(
          label: 'Edit settings',
          kind: BtnKind.secondary,
          onTap: _busy ? null : () => push(EditPage(tunnel: tunnel)),
        ),
        const GroupLabel('Removal', index: 2),
        Btn(
          label: 'Remove tunnel',
          kind: BtnKind.ghost,
          onTap: _busy ? null : _remove,
        ),
      ],
    );
  }

  String _explain(Summary s) {
    switch (s.mode) {
      case 'cathedral':
        if (!s.p2p) {
          return 'A tunnel to a single peer, relayed via a cathedral.';
        }

        return 'A tunnel to a single peer, relayed via a cathedral until '
            'we can talk to it directly.';
      case 'gospel':
        return 'Make calls and send text messages to anyone in the '
            'configured Gospel flock.';
      case 'liturgy':
        if (s.tier6) {
          return 'A full-mesh L2 network, tunnels to all nodes are set up '
              'automatically.';
        }

        return 'A full-mesh network, tunnels to all peers are set up '
            'automatically.';
      default:
        return 'A direct tunnel to a single peer.';
    }
  }
}
