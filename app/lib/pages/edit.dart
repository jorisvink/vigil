import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app.dart';
import '../vigil.dart';
import '../theme.dart';
import '../widgets/buttons.dart';
import '../widgets/ui.dart';

class EditPage extends StatefulWidget {
  const EditPage({super.key, required this.tunnel});

  final Tunnel tunnel;

  @override
  State<EditPage> createState() => _EditPageState();
}

class _EditPageState extends State<EditPage> {
  late final TextEditingController _name = TextEditingController(
    text: widget.tunnel.summary.instance,
  );
  late final TextEditingController _routes = TextEditingController(
    text: widget.tunnel.summary.routes.join('\n'),
  );
  late final TextEditingController _dns = TextEditingController(
    text: widget.tunnel.summary.dns.join('\n'),
  );
  late bool _p2p = widget.tunnel.summary.p2p;
  late bool _tier6 = widget.tunnel.summary.tier6;
  late bool _commixtion = widget.tunnel.summary.commixtion;
  bool _busy = false;

  Summary get _summary => widget.tunnel.summary;

  bool get _hasRoutes =>
      _summary.mode == 'tunnel' || _summary.mode == 'cathedral';

  bool get _hasDns => !_summary.voice;

  bool get _hasP2p => _summary.cathedral != null;

  bool get _hasTier6 => _summary.mode == 'liturgy';

  bool get _hasCommixtion => _summary.cathedral != null && _summary.shroud;

  bool get _active {
    final status = Vigil.instance.status;
    if (_summary.voice) {
      return status.running;
    }

    return status.running && status.tunnelId == widget.tunnel.id;
  }

  @override
  void dispose() {
    _name.dispose();
    _routes.dispose();
    _dns.dispose();
    super.dispose();
  }

  List<String> _entries(TextEditingController text) =>
      text.text.split(RegExp(r'[\s,]+')).where((e) => e.isNotEmpty).toList();

  Future<void> _save() async {
    final name = _name.text.trim();

    if (name.isEmpty) {
      notify('Give the tunnel a name.');
      return;
    }

    setState(() => _busy = true);

    try {
      final active = _active;
      final running = Vigil.instance.status.tunnelId;

      await Vigil.instance.edit(
        widget.tunnel.id,
        instance: name,
        routes: _hasRoutes ? _entries(_routes) : null,
        dns: _hasDns ? _entries(_dns) : const [],
        p2p: _hasP2p ? _p2p && !_commixtion : null,
        tier6: _hasTier6 ? _tier6 : null,
        commixtion: _hasCommixtion ? _commixtion : null,
      );

      if (active && running != null) {
        await Vigil.instance.connect(running);
      }

      navigatorKey.currentState?.maybePop();
      notify(
        active ? 'Settings saved, reconnecting.' : 'Settings saved.',
        warn: false,
      );
    } on VigilError catch (e) {
      notify(e.message);
      if (mounted) {
        setState(() => _busy = false);
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    var index = 0;

    return PushedPage(
      rubric: 'Edit',
      title: _summary.instance,
      bottom: Btn(label: 'Save', expand: true, busy: _busy, onTap: _save),
      children: [
        GroupLabel('Name', index: ++index),
        Pre(
          child: TextField(
            controller: _name,
            maxLength: _nameMax,
            autocorrect: false,
            enableSuggestions: false,
            textInputAction: TextInputAction.done,
            inputFormatters: [
              FilteringTextInputFormatter.allow(RegExp(r'[A-Za-z0-9 ._-]')),
            ],
            style: Styles.pre,
            cursorColor: Palette.amber,
            decoration: InputDecoration.collapsed(
              hintText: 'office',
              hintStyle: Styles.pre.copyWith(color: Palette.dim),
            ).copyWith(counterText: ''),
          ),
        ),
        const Text(
          'Letters, digits, spaces and . _ - only.',
          style: Styles.detail,
        ),
        if (_hasRoutes) ...[
          GroupLabel('Routes', index: ++index),
          _Entries(controller: _routes, hint: '0.0.0.0/0'),
          const Text(
            'One network per line, these are sent via the tunnel. '
            '0.0.0.0/0 sends all traffic via the tunnel.',
            style: Styles.detail,
          ),
        ],
        if (_hasDns) ...[
          GroupLabel('DNS', index: ++index),
          _Entries(controller: _dns, hint: '10.0.0.1'),
          const Text('One server per line, up to four.', style: Styles.detail),
        ],
        if (_hasP2p && !_commixtion) ...[
          GroupLabel('Peer to peer', index: ++index),
          Panel(
            padding: const EdgeInsets.fromLTRB(16, 14, 6, 14),
            child: Row(
              children: [
                const Expanded(
                  child: Text(
                    'Enables P2P - turn off if you want to relay via a '
                    'cathedral instead.',
                    style: Styles.body,
                  ),
                ),
                Toggle(
                  on: _p2p,
                  label: 'Peer to peer',
                  onTap: () => setState(() => _p2p = !_p2p),
                ),
              ],
            ),
          ),
        ],
        if (_hasCommixtion) ...[
          GroupLabel('Commixtion', index: ++index),
          Panel(
            padding: const EdgeInsets.fromLTRB(16, 14, 6, 14),
            child: Row(
              children: [
                const Expanded(
                  child: Text(
                    'Mixes your packets through multiple cathedrals, '
                    'this turns off P2P.',
                    style: Styles.body,
                  ),
                ),
                Toggle(
                  on: _commixtion,
                  label: 'Commixtion',
                  onTap: () => setState(() => _commixtion = !_commixtion),
                ),
              ],
            ),
          ),
        ],
        if (_hasTier6) ...[
          GroupLabel('Tier6', index: ++index),
          Panel(
            padding: const EdgeInsets.fromLTRB(16, 14, 6, 14),
            child: Row(
              children: [
                const Expanded(
                  child: Text(
                    'Enable if you want to use Sanctum its Tier6 daemon (L2)',
                    style: Styles.body,
                  ),
                ),
                Toggle(
                  on: _tier6,
                  label: 'Tier6',
                  onTap: () => setState(() => _tier6 = !_tier6),
                ),
              ],
            ),
          ),
        ],
        if (_active)
          const Notice(text: 'The tunnel reconnects to apply the changes.'),
      ],
    );
  }
}

const _nameMax = 63;

class _Entries extends StatelessWidget {
  const _Entries({required this.controller, required this.hint});

  final TextEditingController controller;
  final String hint;

  @override
  Widget build(BuildContext context) {
    return Pre(
      child: TextField(
        controller: controller,
        minLines: 3,
        maxLines: 10,
        autocorrect: false,
        enableSuggestions: false,
        keyboardType: TextInputType.multiline,
        style: Styles.pre,
        cursorColor: Palette.amber,
        decoration: InputDecoration.collapsed(
          hintText: hint,
          hintStyle: Styles.pre.copyWith(color: Palette.dim),
        ),
      ),
    );
  }
}
