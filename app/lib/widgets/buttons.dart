// Buttons and switches, no ripples. They change colour on press like
// the site does on hover.

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import '../theme.dart';
import 'marks.dart';

class Pressable extends StatefulWidget {
  const Pressable({super.key, required this.builder, this.onTap, this.label});

  final Widget Function(BuildContext context, bool pressed) builder;
  final VoidCallback? onTap;
  final String? label;

  @override
  State<Pressable> createState() => _PressableState();
}

class _PressableState extends State<Pressable> {
  bool _pressed = false;

  void _set(bool value) {
    if (_pressed != value) {
      setState(() => _pressed = value);
    }
  }

  @override
  Widget build(BuildContext context) {
    final enabled = widget.onTap != null;

    return Semantics(
      button: true,
      enabled: enabled,
      label: widget.label,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTapDown: enabled ? (_) => _set(true) : null,
        onTapUp: enabled ? (_) => _set(false) : null,
        onTapCancel: enabled ? () => _set(false) : null,
        onTap: enabled
            ? () {
                HapticFeedback.selectionClick();
                widget.onTap!();
              }
            : null,
        child: widget.builder(context, _pressed),
      ),
    );
  }
}

enum BtnKind { primary, secondary, ghost }

class Btn extends StatelessWidget {
  const Btn({
    super.key,
    required this.label,
    this.onTap,
    this.kind = BtnKind.primary,
    this.glyph,
    this.expand = false,
    this.busy = false,
  });

  final String label;
  final VoidCallback? onTap;
  final BtnKind kind;
  final GlyphKind? glyph;

  final bool expand;

  final bool busy;

  @override
  Widget build(BuildContext context) {
    final enabled = onTap != null && !busy;

    return Pressable(
      onTap: enabled ? onTap : null,
      label: label,
      builder: (context, pressed) {
        Color fill;
        Color stroke;
        Color fg;

        switch (kind) {
          case BtnKind.primary:
            fill = !enabled
                ? Palette.amberDim
                : pressed
                ? Palette.amber2
                : Palette.amber;
            stroke = fill;
            fg = Palette.bg;
          case BtnKind.secondary:
            fill = pressed ? Palette.bg3 : Palette.bg2;
            stroke = pressed ? Palette.amber : Palette.line2;
            fg = enabled ? Palette.fg : Palette.dim;
          case BtnKind.ghost:
            fill = const Color(0x00000000);
            stroke = fill;
            fg = !enabled
                ? Palette.dim
                : pressed
                ? Palette.amber2
                : Palette.amber;
        }

        final text = Text(
          busy ? 'Working…'.toUpperCase() : label.toUpperCase(),
          style: Styles.btn.copyWith(color: fg),
          overflow: TextOverflow.ellipsis,
        );

        return CustomPaint(
          painter: CutPainter(fill: fill, stroke: stroke),
          child: Container(
            height: kind == BtnKind.ghost ? 40 : 52,
            padding: EdgeInsets.symmetric(
              horizontal: kind == BtnKind.ghost ? 4 : 20,
            ),
            child: Row(
              mainAxisSize: expand ? MainAxisSize.max : MainAxisSize.min,
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                if (glyph != null) ...[
                  Glyph(glyph!, size: 18, color: fg),
                  const SizedBox(width: 10),
                ],
                Flexible(child: text),
              ],
            ),
          ),
        );
      },
    );
  }
}

class BtnPair extends StatelessWidget {
  const BtnPair({super.key, required this.first, required this.second});

  final Widget first;
  final Widget second;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Expanded(child: first),
        const SizedBox(width: 10),
        Expanded(child: second),
      ],
    );
  }
}

class Toggle extends StatelessWidget {
  const Toggle({
    super.key,
    required this.on,
    this.pending = false,
    this.onTap,
    this.label,
  });

  final bool on;

  final String? label;

  final bool pending;
  final VoidCallback? onTap;

  static const double _width = 54;
  static const double _height = 30;
  static const double _knob = 20;

  @override
  Widget build(BuildContext context) {
    return Pressable(
      onTap: onTap,
      label: label ?? (on ? 'Disconnect' : 'Connect'),
      builder: (context, pressed) {
        final accent = pending ? Palette.amberDim : Palette.amber;

        return SizedBox(
          width: _width + 12,
          height: 44,
          child: Center(
            child: TweenAnimationBuilder<double>(
              tween: Tween(end: on ? 1 : 0),
              duration: const Duration(milliseconds: 160),
              curve: Curves.easeOut,
              builder: (context, t, _) => CustomPaint(
                painter: CutPainter(
                  fill: Color.lerp(Palette.bg3, Palette.amberWash, t)!,
                  stroke: Color.lerp(
                    pressed ? Palette.fg2 : Palette.line2,
                    accent,
                    t,
                  )!,
                  cut: 6,
                ),
                child: SizedBox(
                  width: _width,
                  height: _height,
                  child: Padding(
                    padding: const EdgeInsets.all(5),
                    child: Align(
                      alignment: Alignment(t * 2 - 1, 0),
                      child: Container(
                        width: _knob,
                        height: _knob,
                        color: Color.lerp(Palette.dim, accent, t),
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
        );
      },
    );
  }
}

class GlyphBtn extends StatelessWidget {
  const GlyphBtn(this.glyph, {super.key, required this.label, this.onTap});

  final GlyphKind glyph;
  final String label;
  final VoidCallback? onTap;

  Color _color(bool pressed) {
    if (onTap == null) {
      return Palette.dim;
    }

    if (pressed) {
      return Palette.amber;
    }

    return Palette.fg2;
  }

  @override
  Widget build(BuildContext context) {
    return Pressable(
      onTap: onTap,
      label: label,
      builder: (context, pressed) => SizedBox(
        width: 44,
        height: 44,
        child: Center(child: Glyph(glyph, color: _color(pressed))),
      ),
    );
  }
}
