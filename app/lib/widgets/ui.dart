import 'dart:math' as math;

import 'package:flutter/widgets.dart';

import '../theme.dart';
import 'buttons.dart';
import 'marks.dart';
import 'net_data.dart';

double tabBarSpace(BuildContext context) =>
    Metrics.tabBar + MediaQuery.paddingOf(context).bottom + 24;

class DotGrid extends StatelessWidget {
  const DotGrid({super.key, required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context) {
    return CustomPaint(painter: const _DotsPainter(), child: child);
  }
}

class _DotsPainter extends CustomPainter {
  const _DotsPainter();

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(Offset.zero & size, Paint()..color = Palette.bg);

    final paint = Paint()..color = const Color(0x0dffffff);

    for (double y = 12; y < size.height; y += 24) {
      for (double x = 12; x < size.width; x += 24) {
        canvas.drawCircle(Offset(x, y), 1.15, paint);
      }
    }
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}

class TopBar extends StatelessWidget {
  const TopBar({super.key, this.actions = const [], this.glow = false});

  final List<Widget> actions;

  final bool glow;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      height: 56,
      child: Row(
        children: [
          const SizedBox(width: Metrics.edge),
          WordmarkSquare(glow: glow),
          const SizedBox(width: 12),
          Text('VIGIL', style: Styles.wordmark),
          const Spacer(),
          ...actions,
          const SizedBox(width: Metrics.edge - 10),
        ],
      ),
    );
  }
}

class Rubric extends StatelessWidget {
  const Rubric({super.key, required this.text, this.index});

  final String text;
  final int? index;

  @override
  Widget build(BuildContext context) {
    return Text.rich(
      TextSpan(
        children: [
          if (index != null)
            TextSpan(
              text: '${hex(index!)}   ',
              style: const TextStyle(color: Palette.amber),
            ),
          TextSpan(text: text.toUpperCase()),
        ],
      ),
      style: Styles.rubric,
    );
  }
}

class PageTitle extends StatelessWidget {
  const PageTitle({
    super.key,
    required this.rubric,
    required this.title,
    required this.index,
  });

  final String rubric;
  final String title;
  final int index;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(Metrics.edge, 18, Metrics.edge, 22),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const RubricCross(),
              const SizedBox(width: 12),
              Rubric(text: rubric, index: index),
            ],
          ),
          const SizedBox(height: 10),
          Text(title, style: Styles.title),
        ],
      ),
    );
  }
}

class TabPage extends StatelessWidget {
  const TabPage({
    super.key,
    required this.rubric,
    required this.title,
    required this.index,
    required this.children,
    this.actions = const [],
    this.glow = false,
  });

  final String rubric;
  final String title;
  final int index;
  final List<Widget> children;
  final List<Widget> actions;
  final bool glow;

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      bottom: false,
      child: Column(
        children: [
          TopBar(actions: actions, glow: glow),
          const Line(),
          Expanded(
            child: ListView(
              padding: EdgeInsets.only(bottom: tabBarSpace(context)),
              children: [
                PageTitle(rubric: rubric, title: title, index: index),
                for (final child in children)
                  Padding(
                    padding: const EdgeInsets.fromLTRB(
                      Metrics.edge,
                      0,
                      Metrics.edge,
                      Metrics.gap,
                    ),
                    child: child,
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class PushedPage extends StatelessWidget {
  const PushedPage({
    super.key,
    required this.title,
    required this.children,
    this.rubric,
    this.bottom,
    this.actions = const [],
  });

  final String title;
  final String? rubric;
  final List<Widget> children;
  final Widget? bottom;
  final List<Widget> actions;

  @override
  Widget build(BuildContext context) {
    final keyboard = MediaQuery.viewInsetsOf(context).bottom;
    final inset = keyboard > 0 ? 0.0 : MediaQuery.paddingOf(context).bottom;

    return DotGrid(
      child: Padding(
        padding: EdgeInsets.only(bottom: keyboard),
        child: SafeArea(
          bottom: false,
          child: Column(
            children: [
              SizedBox(
                height: 56,
                child: Row(
                  children: [
                    const SizedBox(width: Metrics.edge - 12),
                    GlyphBtn(
                      GlyphKind.back,
                      label: 'Back',
                      onTap: () => Navigator.of(context).maybePop(),
                    ),
                    const SizedBox(width: 2),
                    Expanded(
                      child: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          if (rubric != null) ...[
                            Rubric(text: rubric!),
                            const SizedBox(height: 2),
                          ],
                          Text(
                            title,
                            style: Styles.barTitle,
                            overflow: TextOverflow.ellipsis,
                          ),
                        ],
                      ),
                    ),
                    ...actions,
                    const SizedBox(width: Metrics.edge - 10),
                  ],
                ),
              ),
              const Line(),
              Expanded(
                child: ListView(
                  padding: EdgeInsets.fromLTRB(
                    Metrics.edge,
                    Metrics.edge,
                    Metrics.edge,
                    bottom == null ? inset + 24 : 24,
                  ),
                  children: [
                    for (final child in children)
                      Padding(
                        padding: const EdgeInsets.only(bottom: Metrics.gap),
                        child: child,
                      ),
                  ],
                ),
              ),
              if (bottom != null)
                Container(
                  decoration: const BoxDecoration(
                    color: Palette.bg2,
                    border: Border(top: BorderSide(color: Palette.line)),
                  ),
                  padding: EdgeInsets.fromLTRB(
                    Metrics.edge,
                    14,
                    Metrics.edge,
                    inset + 14,
                  ),
                  child: bottom,
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class Line extends StatelessWidget {
  const Line({super.key, this.color = Palette.line});

  final Color color;

  @override
  Widget build(BuildContext context) {
    return Container(height: 1, color: color);
  }
}

class Panel extends StatelessWidget {
  const Panel({
    super.key,
    required this.child,
    this.accent = false,
    this.onTap,
    this.padding = const EdgeInsets.all(18),
  });

  final Widget child;

  final bool accent;
  final VoidCallback? onTap;
  final EdgeInsets padding;

  @override
  Widget build(BuildContext context) {
    Widget panel(bool pressed) => CustomPaint(
      painter: CutPainter(
        fill: pressed ? Palette.bg3 : Palette.bg2,
        stroke: accent
            ? Palette.amber
            : pressed
            ? Palette.line2
            : Palette.line,
      ),
      child: Padding(padding: padding, child: child),
    );

    if (onTap == null) {
      return panel(false);
    }

    return Pressable(
      onTap: onTap,
      builder: (context, pressed) => panel(pressed),
    );
  }
}

class GroupLabel extends StatelessWidget {
  const GroupLabel(this.text, {super.key, this.index, this.trailing});

  final String text;
  final int? index;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(top: 12),
      child: Row(
        children: [
          Expanded(
            child: Rubric(text: text, index: index),
          ),
          ?trailing,
        ],
      ),
    );
  }
}

enum Liveness { off, pending, online }

class StateDot extends StatelessWidget {
  const StateDot(this.state, {super.key, this.size = 8});

  final Liveness state;
  final double size;

  @override
  Widget build(BuildContext context) {
    final color = switch (state) {
      Liveness.off => Palette.line2,
      Liveness.pending => Palette.amber,
      Liveness.online => Palette.good,
    };

    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        color: color,
        boxShadow: state == Liveness.off
            ? null
            : [BoxShadow(color: color.withValues(alpha: 0.55), blurRadius: 8)],
      ),
    );
  }
}

class AroundDot extends StatefulWidget {
  const AroundDot({super.key, this.size = 8});

  final double size;

  @override
  State<AroundDot> createState() => _AroundDotState();
}

class _AroundDotState extends State<AroundDot>
    with SingleTickerProviderStateMixin {
  late final AnimationController _clock = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 4000),
  );

  static double _blip(double p) {
    if (p < 0.9375) {
      return 0;
    }

    if (p < 0.9575) {
      return (p - 0.9375) / 0.02;
    }

    return 1 - (p - 0.9575) / 0.0425;
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();

    if (MediaQuery.disableAnimationsOf(context)) {
      _clock.stop();
      _clock.value = 0;
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
    return RepaintBoundary(
      child: AnimatedBuilder(
        animation: _clock,
        builder: (context, _) {
          final t = _blip(_clock.value);

          return Container(
            width: widget.size,
            height: widget.size,
            decoration: BoxDecoration(
              color: Palette.good.withValues(alpha: 0.9 * t),
              border: Border.all(color: Palette.good.withValues(alpha: 0.75)),
              boxShadow: t == 0
                  ? null
                  : [
                      BoxShadow(
                        color: Palette.good.withValues(alpha: 0.55 * t),
                        blurRadius: 8,
                      ),
                    ],
            ),
          );
        },
      ),
    );
  }
}

class Tag extends StatelessWidget {
  const Tag(this.text, {super.key, this.accent = false});

  final String text;
  final bool accent;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 4),
      decoration: BoxDecoration(
        color: accent ? Palette.amberWash : null,
        border: Border.all(color: accent ? Palette.amberDim : Palette.line2),
      ),
      child: Text(
        text.toUpperCase(),
        style: Styles.rubric.copyWith(
          fontSize: 9.5,
          height: 1,
          color: accent ? Palette.amber : Palette.dim,
        ),
      ),
    );
  }
}

class KeyList extends StatelessWidget {
  const KeyList({super.key, required this.rows});

  final List<(String, String)> rows;

  @override
  Widget build(BuildContext context) {
    return Panel(
      padding: EdgeInsets.zero,
      child: Column(
        children: [
          for (var i = 0; i < rows.length; i++) ...[
            if (i > 0) const Line(),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 13),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  SizedBox(
                    width: 118,
                    child: Padding(
                      padding: const EdgeInsets.only(top: 1),
                      child: Text(rows[i].$1.toUpperCase(), style: Styles.key),
                    ),
                  ),
                  Expanded(
                    child: Text(
                      rows[i].$2,
                      style: Styles.value,
                      textAlign: TextAlign.right,
                    ),
                  ),
                ],
              ),
            ),
          ],
        ],
      ),
    );
  }
}

class LinkRow {
  const LinkRow(this.label, this.onTap, {this.detail});

  final String label;
  final String? detail;
  final VoidCallback onTap;
}

class LinkList extends StatelessWidget {
  const LinkList({super.key, required this.rows});

  final List<LinkRow> rows;

  @override
  Widget build(BuildContext context) {
    return Panel(
      padding: EdgeInsets.zero,
      child: Column(
        children: [
          for (var i = 0; i < rows.length; i++) ...[
            if (i > 0) const Line(),
            Pressable(
              onTap: rows[i].onTap,
              label: rows[i].label,
              builder: (context, pressed) => Container(
                color: pressed ? Palette.bg3 : null,
                padding: const EdgeInsets.fromLTRB(16, 14, 10, 14),
                child: Row(
                  children: [
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(
                            rows[i].label,
                            style: Styles.body.copyWith(
                              color: pressed ? Palette.amber : Palette.fg,
                            ),
                          ),
                          if (rows[i].detail != null)
                            Text(rows[i].detail!, style: Styles.detail),
                        ],
                      ),
                    ),
                    Glyph(
                      GlyphKind.chevron,
                      size: 18,
                      color: pressed ? Palette.amber : Palette.dim,
                    ),
                  ],
                ),
              ),
            ),
          ],
        ],
      ),
    );
  }
}

class StatTile extends StatelessWidget {
  const StatTile({
    super.key,
    required this.label,
    required this.value,
    this.detail,
  });

  final String label;
  final String value;
  final String? detail;

  @override
  Widget build(BuildContext context) {
    return Panel(
      padding: const EdgeInsets.fromLTRB(16, 14, 16, 14),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label.toUpperCase(), style: Styles.key),
          const SizedBox(height: 8),
          FittedBox(
            fit: BoxFit.scaleDown,
            alignment: Alignment.centerLeft,
            child: Text(value, style: Styles.figure),
          ),
          if (detail != null) ...[
            const SizedBox(height: 4),
            Text(detail!, style: Styles.detail, maxLines: 1),
          ],
        ],
      ),
    );
  }
}

class StatGrid extends StatelessWidget {
  const StatGrid({super.key, required this.tiles});

  final List<Widget> tiles;

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        for (var i = 0; i < tiles.length; i += 2) ...[
          if (i > 0) const SizedBox(height: 10),
          IntrinsicHeight(
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Expanded(child: tiles[i]),
                const SizedBox(width: 10),
                Expanded(
                  child: i + 1 < tiles.length
                      ? tiles[i + 1]
                      : const SizedBox.shrink(),
                ),
              ],
            ),
          ),
        ],
      ],
    );
  }
}

enum NoticeKind { note, warn, good }

class Notice extends StatelessWidget {
  const Notice({super.key, required this.text, this.kind = NoticeKind.note});

  final String text;
  final NoticeKind kind;

  @override
  Widget build(BuildContext context) {
    final (tag, edge) = switch (kind) {
      NoticeKind.note => ('NOTE', Palette.line2),
      NoticeKind.warn => ('WARNING', Palette.amber),
      NoticeKind.good => ('OK', Palette.good),
    };

    return Container(
      decoration: BoxDecoration(
        color: Palette.bg2,
        border: Border(
          left: BorderSide(color: edge, width: 2),
          top: const BorderSide(color: Palette.line),
          right: const BorderSide(color: Palette.line),
          bottom: const BorderSide(color: Palette.line),
        ),
      ),
      padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
      child: Text.rich(
        TextSpan(
          children: [
            TextSpan(
              text: '$tag   ',
              style: Styles.rubric.copyWith(
                fontSize: 10.5,
                color: kind == NoticeKind.note ? Palette.dim : edge,
              ),
            ),
            TextSpan(text: text),
          ],
        ),
        style: Styles.notice,
      ),
    );
  }
}

class Pre extends StatelessWidget {
  const Pre({super.key, required this.child});

  Pre.text(String text, {super.key}) : child = Text(text, style: Styles.pre);

  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Container(
      decoration: const BoxDecoration(
        color: Color(0xff07080a),
        border: Border(
          left: BorderSide(color: Palette.amber, width: 2),
          top: BorderSide(color: Palette.line),
          right: BorderSide(color: Palette.line),
          bottom: BorderSide(color: Palette.line),
        ),
      ),
      padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
      child: child,
    );
  }
}

class EmptyState extends StatelessWidget {
  const EmptyState({
    super.key,
    required this.title,
    required this.text,
    this.glyph = GlyphKind.shield,
    this.action,
  });

  final String title;
  final String text;
  final GlyphKind glyph;
  final Widget? action;

  @override
  Widget build(BuildContext context) {
    return Panel(
      padding: const EdgeInsets.fromLTRB(22, 30, 22, 24),
      child: Column(
        children: [
          Glyph(glyph, size: 36, color: Palette.amber),
          const SizedBox(height: 16),
          Text(title, style: Styles.cardTitle, textAlign: TextAlign.center),
          const SizedBox(height: 8),
          Text(text, style: Styles.body, textAlign: TextAlign.center),
          if (action != null) ...[const SizedBox(height: 20), action!],
        ],
      ),
    );
  }
}

class NetBackdrop extends StatefulWidget {
  const NetBackdrop({super.key, this.intensity = 1, this.centered = false});

  final double intensity;
  final bool centered;

  @override
  State<NetBackdrop> createState() => _NetBackdropState();
}

class _NetBackdropState extends State<NetBackdrop>
    with SingleTickerProviderStateMixin {
  late final AnimationController _clock;

  @override
  void initState() {
    super.initState();

    _clock = AnimationController(
      vsync: this,
      duration: const Duration(minutes: 10),
    );
  }

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
    return IgnorePointer(
      child: ClipRect(
        child: CustomPaint(
          painter: _NetPainter(_clock, widget.intensity, widget.centered),
        ),
      ),
    );
  }
}

class _NetPainter extends CustomPainter {
  _NetPainter(this.clock, this.intensity, this.centered)
    : super(repaint: clock);

  final Animation<double> clock;
  final double intensity;
  final bool centered;

  /// @keyframes line-pulse, 0.12 except for a flash to 0.6 at 90%.
  static double _pulse(double t, double period, double delay) {
    var p = ((t - delay) % period) / period;
    if (p < 0) {
      p += 1;
    }

    if (p < 0.84) {
      return 0.12;
    }

    if (p < 0.90) {
      return 0.12 + (p - 0.84) / 0.06 * 0.48;
    }

    return 0.6 - (p - 0.90) / 0.10 * 0.48;
  }

  double _mask(Offset p, Size size) {
    if (!centered) {
      final x = p.dx / size.width;
      return ((x - 0.3) / 0.6).clamp(0.08, 1.0);
    }

    final x = (p.dx / size.width - 0.5) * 2;
    final y = (p.dy / size.height - 0.5) * 2;
    final r = math.sqrt(x * x + y * y);
    final inner = ((r - 0.3) / 0.35).clamp(0.0, 1.0);
    final edge = math.max(x.abs(), y.abs());
    final outer = ((1 - edge) / 0.15).clamp(0.0, 1.0);

    return inner * outer;
  }

  @override
  void paint(Canvas canvas, Size size) {
    // The right hand side of the 1600x480 viewbox, scaled to cover.
    final scale = math.max(size.width / 900, size.height / 480);
    final ox = size.width - 1600 * scale;
    final oy = (size.height - 480 * scale) / 2;
    final seconds = clock.value * 600;

    Offset map(double x, double y) => Offset(ox + x * scale, oy + y * scale);

    final paint = Paint()
      ..strokeWidth = 1
      ..style = PaintingStyle.stroke;

    for (final l in netLines) {
      final a = map(l[0], l[1]);
      final b = map(l[2], l[3]);
      final mid = Offset((a.dx + b.dx) / 2, (a.dy + b.dy) / 2);

      if (mid.dx < -50 || mid.dx > size.width + 50) {
        continue;
      }

      final alpha = _pulse(seconds, l[4], l[5]) * _mask(mid, size) * intensity;
      paint.color = Palette.amber.withValues(alpha: alpha.clamp(0.0, 1.0));
      canvas.drawLine(a, b, paint);
    }

    final node = Paint();
    for (final n in netNodes) {
      final p = map(n[0], n[1]);
      final alpha = 0.35 * _mask(p, size) * intensity;
      node.color = Palette.amber.withValues(alpha: alpha.clamp(0.0, 1.0));
      canvas.drawRect(Rect.fromLTWH(p.dx, p.dy, 3, 3), node);
    }
  }

  @override
  bool shouldRepaint(covariant _NetPainter oldDelegate) =>
      oldDelegate.intensity != intensity || oldDelegate.centered != centered;
}
