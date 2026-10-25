import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/widgets.dart';

import '../theme.dart';
import '../vigil.dart';

const _detailMax = 24;

class NetworkMap extends StatefulWidget {
  const NetworkMap({super.key, required this.status});

  final Status status;

  @override
  State<NetworkMap> createState() => _NetworkMapState();
}

class _NetworkMapState extends State<NetworkMap>
    with SingleTickerProviderStateMixin {
  late final AnimationController _clock = AnimationController(
    vsync: this,
    duration: const Duration(minutes: 10),
  );

  String? _focus;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();

    if (MediaQuery.disableAnimationsOf(context)) {
      _clock.stop();
    } else if (!_clock.isAnimating) {
      _clock.repeat();
    }
  }

  @override
  void dispose() {
    _clock.dispose();
    super.dispose();
  }

  void _tap(Offset at, Size size, _Network net) {
    if (_focus != null) {
      setState(() => _focus = null);
      return;
    }

    if (!net.summary) {
      return;
    }

    final layout = _Layout(net, size);
    String? hit;
    var best = 32.0;

    for (final c in net.cathedrals.values) {
      if (c.peers == 0) {
        continue;
      }

      for (final p in [
        layout.cathedrals[c.address]!,
        layout.clusters[c.address]!,
      ]) {
        final d = (p - at).distance;
        if (d < best) {
          best = d;
          hit = c.address;
        }
      }
    }

    if (hit != null) {
      setState(() => _focus = hit);
    }
  }

  @override
  Widget build(BuildContext context) {
    var net = _Network.of(widget.status, _focus);

    if (_focus != null && net.peers.isEmpty) {
      _focus = null;
      net = _Network.of(widget.status, null);
    }

    final online = net.peers.where((p) => p.online).length;

    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        AspectRatio(
          aspectRatio: 1,
          child: LayoutBuilder(
            builder: (context, box) => GestureDetector(
              behavior: HitTestBehavior.opaque,
              onTapUp: (d) => _tap(d.localPosition, box.biggest, net),
              child: CustomPaint(painter: _MapPainter(_clock, net)),
            ),
          ),
        ),
        const SizedBox(height: 14),
        Wrap(
          spacing: 18,
          runSpacing: 8,
          children: [
            const _Key('P2P', Palette.good),
            const _Key('Relayed', Palette.amber),
            const _Key('Waiting', Palette.dim, stroke: _Stroke.dashed),
            _Key(
              _focus != null
                  ? '$online/${net.peers.length} peers via '
                        '${_focus!.split(':').first} · tap to go back'
                  : net.summary
                  ? '$online/${net.peers.length} peers · '
                        '${net.cathedrals.length} cathedrals · '
                        'tap a cathedral'
                  : '$online/${net.peers.length} peers · '
                        '${net.cathedrals.length} cathedrals',
              null,
            ),
          ],
        ),
      ],
    );
  }
}

class CallMap extends StatefulWidget {
  const CallMap({super.key, required this.peer, required this.voice});

  final String peer;
  final VoiceStatus voice;

  @override
  State<CallMap> createState() => _CallMapState();
}

class _CallMapState extends State<CallMap> with SingleTickerProviderStateMixin {
  late final AnimationController _clock = AnimationController(
    vsync: this,
    duration: const Duration(minutes: 10),
  );

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();

    if (MediaQuery.disableAnimationsOf(context)) {
      _clock.stop();
    } else if (!_clock.isAnimating) {
      _clock.repeat();
    }
  }

  @override
  void dispose() {
    _clock.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final voice = widget.voice;
    final net = _Network.call(widget.peer, voice);
    final online = voice.state != 'connecting';
    final cathedral = (voice.cathedral ?? '').split(':').first;

    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        AspectRatio(
          aspectRatio: 2.4,
          child: CustomPaint(painter: _CallPainter(_clock, net)),
        ),
        const SizedBox(height: 10),
        Center(
          child: !online
              ? const _Key('Waiting', Palette.dim, stroke: _Stroke.dashed)
              : voice.p2p
              ? _Key('P2P · ${voice.path ?? ''}', Palette.good)
              : _Key('Relayed via $cathedral', Palette.amber),
        ),
      ],
    );
  }
}

class _Cathedral {
  _Cathedral(this.address);

  final String address;
  bool liturgy = false;
  bool alive = false;
  int peers = 0;
  int p2p = 0;
  int relayed = 0;
  int waiting = 0;
  double angle = 0;

  bool get used => liturgy || peers > 0;
}

class _Network {
  _Network(this.focused);

  final bool focused;
  final cathedrals = <String, _Cathedral>{};
  final peers = <PeerStatus>[];

  bool get summary => !focused && peers.length > _detailMax;

  factory _Network.call(String peer, VoiceStatus voice) {
    final net = _Network(true);
    final address = voice.cathedral ?? '';
    final online = voice.state != 'connecting';

    net.cathedrals[address] = _Cathedral(address)
      ..alive = true
      ..peers = 1
      ..angle = -math.pi / 2;

    net.peers.add(
      PeerStatus({
        'id': peer,
        'online': online,
        'p2p': voice.p2p,
        'cathedral': address,
        'cathedral_alive': true,
      }),
    );

    return net;
  }

  factory _Network.of(Status status, String? focus) {
    final net = _Network(focus != null);

    _Cathedral at(String address) =>
        net.cathedrals.putIfAbsent(address, () => _Cathedral(address));

    final liturgy = status.liturgyCathedral;
    if (liturgy != null && (focus == null || focus == liturgy)) {
      at(liturgy)
        ..liturgy = true
        ..alive = status.liturgyAlive ?? false;
    }

    if (focus == null) {
      for (final r in status.remembrances) {
        at(r);
      }
    }

    for (final p in status.peers) {
      final c = p.cathedral;
      if (c == null || (focus != null && c != focus)) {
        continue;
      }

      final cathedral = at(c);
      cathedral.peers++;

      if (!p.online) {
        cathedral.waiting++;
      } else if (p.p2p) {
        cathedral.p2p++;
      } else {
        cathedral.relayed++;
      }

      if (p.cathedralAlive) {
        cathedral.alive = true;
      }
      net.peers.add(p);
    }

    final order = net.cathedrals.keys.toList();
    final count = math.max(order.length, 1);

    for (var i = 0; i < order.length; i++) {
      net.cathedrals[order[i]]!.angle = -math.pi / 2 + 2 * math.pi * i / count;
    }

    int id(PeerStatus p) => int.parse(p.id, radix: 16);
    int via(PeerStatus p) => order.indexOf(p.cathedral ?? '');

    net.peers.sort((a, b) {
      final c = via(a) - via(b);
      if (c != 0) {
        return c;
      }

      return id(a) - id(b);
    });

    return net;
  }
}

class _Layout {
  _Layout(this.net, Size size)
    : line = false,
      center = Offset(size.width / 2, size.height / 2),
      inner = math.min(size.width, size.height) * 0.24,
      outer = math.min(size.width, size.height) * 0.41 {
    for (final c in net.cathedrals.values) {
      cathedrals[c.address] = on(inner, c.angle);
      clusters[c.address] = on(outer, c.angle);
    }

    if (net.summary) {
      return;
    }

    final step = 2 * math.pi / math.max(net.peers.length, 1);
    final turn = _turn(step);

    for (var i = 0; i < net.peers.length; i++) {
      peers[net.peers[i].id] = on(outer, turn + step * i);
    }
  }

  _Layout.call(this.net, Size size)
    : center = Offset(size.width * 0.14, size.height * 0.7),
      inner = 0,
      outer = 0,
      line = true {
    for (final c in net.cathedrals.values) {
      cathedrals[c.address] = Offset(size.width / 2, size.height * 0.22);
    }

    for (final p in net.peers) {
      peers[p.id] = Offset(size.width * 0.86, size.height * 0.7);
    }
  }

  final _Network net;
  final bool line;
  final Offset center;
  final double inner;
  final double outer;
  final cathedrals = <String, Offset>{};
  final clusters = <String, Offset>{};
  final peers = <String, Offset>{};

  Offset on(double radius, double angle) =>
      center + Offset(math.cos(angle), math.sin(angle)) * radius;

  Offset outward(Offset at) {
    if (line) {
      if (at.dy < center.dy) {
        return const Offset(0, -1);
      }

      return const Offset(1, 0);
    }

    final out = at - center;
    return out / math.max(out.distance, 1);
  }

  static Offset seed(Offset at, int i) {
    final r = 3.6 * math.sqrt(i + 0.5);
    final a = i * 2.39996;
    return at + Offset(math.cos(a), math.sin(a)) * r;
  }

  static double spread(int count) => 3.6 * math.sqrt(count + 0.5) + 3;

  double _turn(double step) {
    var sin = 0.0;
    var cos = 0.0;

    for (var i = 0; i < net.peers.length; i++) {
      final c = net.cathedrals[net.peers[i].cathedral];
      if (c == null) {
        continue;
      }

      sin += math.sin(c.angle - step * i);
      cos += math.cos(c.angle - step * i);
    }

    if (sin == 0 && cos == 0) {
      return -math.pi / 2;
    }

    return math.atan2(sin, cos);
  }
}

class _MapPainter extends CustomPainter {
  _MapPainter(this.clock, this.net) : super(repaint: clock);

  final Animation<double> clock;
  final _Network net;

  static final _labels = <String, TextPainter>{};

  @override
  void paint(Canvas canvas, Size size) {
    final layout = _Layout(net, size);
    final seconds = clock.value * 600;

    final rings = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1
      ..color = Palette.line;

    canvas.drawCircle(layout.center, layout.inner, rings);
    canvas.drawCircle(layout.center, layout.outer, rings);

    for (final c in net.cathedrals.values) {
      if (!c.used) {
        continue;
      }

      _line(
        canvas,
        layout.center,
        layout.cathedrals[c.address]!,
        c.alive ? Palette.amber.withValues(alpha: 0.45) : Palette.line2,
        stroke: c.alive ? _Stroke.solid : _Stroke.dashed,
      );
    }

    if (net.summary) {
      _summary(canvas, layout, seconds);
    } else {
      _detail(canvas, layout, seconds);
    }

    for (final c in net.cathedrals.values) {
      _cathedral(canvas, layout, c);
    }

    _self(canvas, layout.center);
  }

  void _detail(Canvas canvas, _Layout layout, double seconds) {
    final center = layout.center;

    for (var i = 0; i < net.peers.length; i++) {
      final p = net.peers[i];
      final at = layout.peers[p.id]!;
      final via = layout.cathedrals[p.cathedral];

      if (p.p2p) {
        if (via != null) {
          _line(canvas, via, at, Palette.amber.withValues(alpha: 0.18));
        }

        if (via != null && p.cathedralAlive) {
          _signal(canvas, center, via, seconds, i);
        }

        final path = _curve(center, at);
        _path(
          canvas,
          path,
          p.online ? Palette.good : Palette.dim,
          stroke: p.online ? _Stroke.solid : _Stroke.dashed,
        );
        if (p.online) {
          _pulses(canvas, path, Palette.good, seconds, i);
        }
      } else if (via != null) {
        _line(
          canvas,
          via,
          at,
          p.online ? Palette.amber : Palette.dim,
          stroke: p.online ? _Stroke.solid : _Stroke.dashed,
        );

        if (p.online) {
          final path = Path()
            ..moveTo(center.dx, center.dy)
            ..lineTo(via.dx, via.dy)
            ..lineTo(at.dx, at.dy);
          _pulses(canvas, path, Palette.amber2, seconds, i);
        }
      }
    }

    final labels = net.peers.length <= _detailMax * 2;

    for (final p in net.peers) {
      final at = layout.peers[p.id]!;
      _peer(canvas, at, p);

      if (labels) {
        _label(
          canvas,
          p.id,
          at + layout.outward(at) * 16,
          p.online ? Palette.fg : Palette.dim,
        );
      }
    }
  }

  void _summary(Canvas canvas, _Layout layout, double seconds) {
    final center = layout.center;
    var i = 0;

    for (final c in net.cathedrals.values) {
      if (c.peers == 0) {
        continue;
      }

      final via = layout.cathedrals[c.address]!;
      final at = layout.clusters[c.address]!;

      if (c.alive && c.p2p > 0) {
        _signal(canvas, center, via, seconds, i);
      }

      if (c.p2p > 0) {
        final path = _curve(center, at);
        _draw(canvas, path, Palette.good, _Stroke.solid, _width(c.p2p));
        _pulses(canvas, path, Palette.good, seconds, i);
      }

      if (c.relayed > 0) {
        _draw(
          canvas,
          Path()
            ..moveTo(via.dx, via.dy)
            ..lineTo(at.dx, at.dy),
          Palette.amber,
          _Stroke.solid,
          _width(c.relayed),
        );

        final path = Path()
          ..moveTo(center.dx, center.dy)
          ..lineTo(via.dx, via.dy)
          ..lineTo(at.dx, at.dy);
        _pulses(canvas, path, Palette.amber2, seconds, i);
      }

      if (c.p2p == 0 && c.relayed == 0) {
        _line(canvas, via, at, Palette.dim, stroke: _Stroke.dashed);
      }

      i++;
    }

    for (final c in net.cathedrals.values) {
      if (c.peers == 0) {
        continue;
      }

      final at = layout.clusters[c.address]!;
      var j = 0;

      for (final p in net.peers) {
        if (p.cathedral != c.address) {
          continue;
        }
        _dot(canvas, _Layout.seed(at, j++), p);
      }

      final label = at + layout.outward(at) * (_Layout.spread(c.peers) + 10);
      _counts(canvas, label, c);
    }
  }

  static double _width(int count) =>
      math.min(1.2 + math.log(count + 1) / math.ln2 * 0.5, 3.2);

  void _signal(Canvas canvas, Offset from, Offset to, double seconds, int i) {
    _pulses(
      canvas,
      Path()
        ..moveTo(from.dx, from.dy)
        ..lineTo(to.dx, to.dy),
      Palette.amber,
      seconds,
      i,
      alpha: 0.3,
      period: 5.2,
    );
  }

  Path _curve(Offset a, Offset b) {
    final mid = Offset((a.dx + b.dx) / 2, (a.dy + b.dy) / 2);
    final d = b - a;
    final normal = Offset(-d.dy, d.dx) * 0.18;

    return Path()
      ..moveTo(a.dx, a.dy)
      ..quadraticBezierTo(mid.dx + normal.dx, mid.dy + normal.dy, b.dx, b.dy);
  }

  void _line(
    Canvas canvas,
    Offset a,
    Offset b,
    Color color, {
    _Stroke stroke = _Stroke.solid,
  }) {
    _path(
      canvas,
      Path()
        ..moveTo(a.dx, a.dy)
        ..lineTo(b.dx, b.dy),
      color,
      stroke: stroke,
    );
  }

  void _path(
    Canvas canvas,
    Path path,
    Color color, {
    _Stroke stroke = _Stroke.solid,
  }) {
    _draw(canvas, path, color, stroke, 1.2);
  }

  void _pulses(
    Canvas canvas,
    Path path,
    Color color,
    double seconds,
    int i, {
    double alpha = 1,
    double period = 2.6,
  }) {
    final metrics = path.computeMetrics().toList();
    final length = metrics.fold<double>(0, (sum, m) => sum + m.length);
    if (length == 0) {
      return;
    }

    final dot = Paint()..color = color.withValues(alpha: alpha);
    final glow = Paint()
      ..color = color.withValues(alpha: 0.35 * alpha)
      ..maskFilter = const ui.MaskFilter.blur(ui.BlurStyle.normal, 4);

    final phase = (seconds / period + i * 0.37) % 1;

    for (final t in [phase, 1 - (phase + 0.5) % 1]) {
      var d = t * length;

      for (final m in metrics) {
        if (d <= m.length) {
          final p = m.getTangentForOffset(d)?.position;
          if (p != null) {
            canvas.drawCircle(p, 5, glow);
            canvas.drawCircle(p, 1.8, dot);
          }
          break;
        }
        d -= m.length;
      }
    }
  }

  void _self(Canvas canvas, Offset at) {
    canvas.drawRect(
      Rect.fromCenter(center: at, width: 26, height: 26),
      Paint()
        ..color = Palette.amber.withValues(alpha: 0.25)
        ..maskFilter = const ui.MaskFilter.blur(ui.BlurStyle.normal, 10),
    );
    canvas.drawRect(
      Rect.fromCenter(center: at, width: 20, height: 20),
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1
        ..color = Palette.amber,
    );
    canvas.drawRect(
      Rect.fromCenter(center: at, width: 10, height: 10),
      Paint()..color = Palette.amber,
    );
  }

  void _cathedral(Canvas canvas, _Layout layout, _Cathedral c) {
    final at = layout.cathedrals[c.address]!;
    final color = !c.used
        ? Palette.line2
        : c.alive
        ? Palette.amber
        : Palette.dim;

    final cross = Path()
      ..moveTo(at.dx - 1.5, at.dy - 7)
      ..lineTo(at.dx + 1.5, at.dy - 7)
      ..lineTo(at.dx + 1.5, at.dy - 3)
      ..lineTo(at.dx + 5, at.dy - 3)
      ..lineTo(at.dx + 5, at.dy)
      ..lineTo(at.dx + 1.5, at.dy)
      ..lineTo(at.dx + 1.5, at.dy + 7)
      ..lineTo(at.dx - 1.5, at.dy + 7)
      ..lineTo(at.dx - 1.5, at.dy)
      ..lineTo(at.dx - 5, at.dy)
      ..lineTo(at.dx - 5, at.dy - 3)
      ..lineTo(at.dx - 1.5, at.dy - 3)
      ..close();

    if (c.used && c.alive) {
      canvas.drawPath(
        cross,
        Paint()
          ..color = Palette.amber.withValues(alpha: 0.4)
          ..maskFilter = const ui.MaskFilter.blur(ui.BlurStyle.normal, 6),
      );
    }

    canvas.drawPath(cross, Paint()..color = Palette.bg2);
    canvas.drawPath(
      cross,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1
        ..color = color,
    );

    if (c.liturgy) {
      canvas.drawCircle(
        at,
        11,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1
          ..color = color.withValues(alpha: 0.6),
      );
    }

    if (!c.used && net.cathedrals.length > 8) {
      return;
    }

    final out = layout.outward(at);
    final side = net.summary && c.peers > 0
        ? Offset(-out.dy, out.dx) * 34
        : out * 20;

    _label(
      canvas,
      c.address.split(':').first,
      at + side,
      c.used ? Palette.fg2 : Palette.dim,
      size: 8.5,
    );
  }

  void _peer(Canvas canvas, Offset at, PeerStatus p) {
    final color = p.online ? Palette.good : Palette.amber;

    if (p.online) {
      canvas.drawRect(
        Rect.fromCenter(center: at, width: 16, height: 16),
        Paint()
          ..color = color.withValues(alpha: 0.35)
          ..maskFilter = const ui.MaskFilter.blur(ui.BlurStyle.normal, 6),
      );
    }

    canvas.drawRect(
      Rect.fromCenter(center: at, width: 9, height: 9),
      Paint()..color = p.online ? color : Palette.bg2,
    );
    canvas.drawRect(
      Rect.fromCenter(center: at, width: 9, height: 9),
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1
        ..color = color,
    );
  }

  void _dot(Canvas canvas, Offset at, PeerStatus p) {
    if (!p.online) {
      canvas.drawCircle(
        at,
        1.6,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 0.8
          ..color = Palette.dim,
      );
      return;
    }

    canvas.drawCircle(
      at,
      1.8,
      Paint()..color = p.p2p ? Palette.good : Palette.amber,
    );
  }

  void _counts(Canvas canvas, Offset at, _Cathedral c) {
    final key = 'counts|${c.p2p}|${c.relayed}|${c.waiting}';

    final painter = _labels.putIfAbsent(key, () {
      final style = TextStyle(fontFamily: Fonts.mono, fontSize: 9.5);
      final spans = <TextSpan>[];

      void add(int n, Color color) {
        if (n == 0) {
          return;
        }

        if (spans.isNotEmpty) {
          spans.add(
            TextSpan(
              text: '·',
              style: style.copyWith(color: Palette.line2),
            ),
          );
        }
        spans.add(
          TextSpan(
            text: '$n',
            style: style.copyWith(color: color),
          ),
        );
      }

      add(c.p2p, Palette.good);
      add(c.relayed, Palette.amber);
      add(c.waiting, Palette.dim);

      return TextPainter(
        text: TextSpan(children: spans),
        textDirection: TextDirection.ltr,
      )..layout();
    });

    painter.paint(canvas, at - Offset(painter.width / 2, painter.height / 2));
  }

  void _label(
    Canvas canvas,
    String text,
    Offset at,
    Color color, {
    double size = 10,
  }) {
    if (_labels.length > 600) {
      _labels.clear();
    }

    final painter = _labels.putIfAbsent(
      '$text|${color.toARGB32()}|$size',
      () => TextPainter(
        text: TextSpan(
          text: text,
          style: TextStyle(
            fontFamily: Fonts.mono,
            fontSize: size,
            letterSpacing: size * 0.06,
            color: color,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout(),
    );

    painter.paint(canvas, at - Offset(painter.width / 2, painter.height / 2));
  }

  @override
  bool shouldRepaint(covariant _MapPainter oldDelegate) => true;
}

class _CallPainter extends _MapPainter {
  _CallPainter(super.clock, super.net);

  @override
  void paint(Canvas canvas, Size size) {
    final layout = _Layout.call(net, size);
    final seconds = clock.value * 600;

    for (final c in net.cathedrals.values) {
      _line(
        canvas,
        layout.center,
        layout.cathedrals[c.address]!,
        Palette.amber.withValues(alpha: 0.45),
      );
    }

    _detail(canvas, layout, seconds);

    for (final c in net.cathedrals.values) {
      _cathedral(canvas, layout, c);
    }

    _self(canvas, layout.center);
  }

  @override
  Path _curve(Offset a, Offset b) => Path()
    ..moveTo(a.dx, a.dy)
    ..lineTo(b.dx, b.dy);
}

enum _Stroke { solid, dashed }

void _draw(
  Canvas canvas,
  Path path,
  Color color,
  _Stroke stroke,
  double width,
) {
  final paint = Paint()
    ..style = PaintingStyle.stroke
    ..strokeWidth = width
    ..color = color;

  if (stroke == _Stroke.solid) {
    canvas.drawPath(path, paint);
    return;
  }

  for (final metric in path.computeMetrics()) {
    for (var d = 0.0; d < metric.length; d += 7) {
      canvas.drawPath(metric.extractPath(d, d + 3.5), paint);
    }
  }
}

class _Key extends StatelessWidget {
  const _Key(this.label, this.color, {this.stroke = _Stroke.solid});

  final String label;
  final Color? color;
  final _Stroke stroke;

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        if (color != null) ...[
          SizedBox(
            width: 18,
            height: 8,
            child: CustomPaint(painter: _KeyPainter(color!, stroke)),
          ),
          const SizedBox(width: 7),
        ],
        Flexible(child: Text(label.toUpperCase(), style: Styles.rubric)),
      ],
    );
  }
}

class _KeyPainter extends CustomPainter {
  _KeyPainter(this.color, this.stroke);

  final Color color;
  final _Stroke stroke;

  @override
  void paint(Canvas canvas, Size size) {
    final y = size.height / 2;

    _draw(
      canvas,
      Path()
        ..moveTo(0, y)
        ..lineTo(size.width, y),
      color,
      stroke,
      1.5,
    );
  }

  @override
  bool shouldRepaint(covariant _KeyPainter oldDelegate) =>
      oldDelegate.color != color || oldDelegate.stroke != stroke;
}
