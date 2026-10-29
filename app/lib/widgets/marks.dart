// The small marks the app is drawn with: the cut corner, the crosses,
// the wordmark square and the line glyphs used as icons.

import 'dart:math' as math;

import 'package:flutter/widgets.dart';

import '../theme.dart';

Path cutPath(Size size, {double cut = Metrics.cut}) {
  return Path()
    ..moveTo(0, 0)
    ..lineTo(size.width - cut, 0)
    ..lineTo(size.width, cut)
    ..lineTo(size.width, size.height)
    ..lineTo(0, size.height)
    ..close();
}

class CutClipper extends CustomClipper<Path> {
  const CutClipper({this.cut = Metrics.cut});

  final double cut;

  @override
  Path getClip(Size size) => cutPath(size, cut: cut);

  @override
  bool shouldReclip(covariant CutClipper oldClipper) => oldClipper.cut != cut;
}

class CutPainter extends CustomPainter {
  const CutPainter({
    this.fill = Palette.bg2,
    this.stroke = Palette.line,
    this.cut = Metrics.cut,
  });

  final Color fill;
  final Color stroke;
  final double cut;

  @override
  void paint(Canvas canvas, Size size) {
    final path = cutPath(size, cut: cut);

    canvas.drawPath(path, Paint()..color = fill);
    canvas.drawPath(
      path,
      Paint()
        ..color = stroke
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1,
    );
  }

  @override
  bool shouldRepaint(covariant CutPainter oldDelegate) =>
      oldDelegate.fill != fill ||
      oldDelegate.stroke != stroke ||
      oldDelegate.cut != cut;
}

/// The outlined cross the site puts next to every rubric.
///
/// The svg path is M3.5 .5h4v3h3v4h-3v7h-4v-7h-3v-4h3z, filled with
/// the background and stroked in amber.
class RubricCross extends StatelessWidget {
  const RubricCross({super.key, this.fill = Palette.bg});

  final Color fill;

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      size: const Size(11, 15),
      painter: _RubricCrossPainter(fill),
    );
  }
}

class _RubricCrossPainter extends CustomPainter {
  const _RubricCrossPainter(this.fill);

  final Color fill;

  @override
  void paint(Canvas canvas, Size size) {
    final path = Path()
      ..moveTo(3.5, 0.5)
      ..relativeLineTo(4, 0)
      ..relativeLineTo(0, 3)
      ..relativeLineTo(3, 0)
      ..relativeLineTo(0, 4)
      ..relativeLineTo(-3, 0)
      ..relativeLineTo(0, 7)
      ..relativeLineTo(-4, 0)
      ..relativeLineTo(0, -7)
      ..relativeLineTo(-3, 0)
      ..relativeLineTo(0, -4)
      ..relativeLineTo(3, 0)
      ..close();

    canvas.drawPath(path, Paint()..color = fill);
    canvas.drawPath(
      path,
      Paint()
        ..color = Palette.amber
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1,
    );
  }

  @override
  bool shouldRepaint(covariant _RubricCrossPainter oldDelegate) =>
      oldDelegate.fill != fill;
}

class SolidCross extends StatelessWidget {
  const SolidCross({super.key, this.glow = 0, this.color = Palette.amber});

  final double glow;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      size: const Size(9, 13),
      painter: _SolidCrossPainter(glow, color),
    );
  }
}

class _SolidCrossPainter extends CustomPainter {
  const _SolidCrossPainter(this.glow, this.color);

  final double glow;
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = Color.lerp(color, const Color(0xffffe9b3), glow * 0.7)!;

    if (glow > 0) {
      final halo = Paint()
        ..color = color.withValues(alpha: 0.9 * glow)
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 3.5);
      canvas.drawRect(const Rect.fromLTWH(3, 0, 3, 13), halo);
      canvas.drawRect(const Rect.fromLTWH(0, 3, 9, 3), halo);
    }

    canvas.drawRect(const Rect.fromLTWH(3, 0, 3, 13), paint);
    canvas.drawRect(const Rect.fromLTWH(0, 3, 9, 3), paint);
  }

  @override
  bool shouldRepaint(covariant _SolidCrossPainter oldDelegate) =>
      oldDelegate.glow != glow || oldDelegate.color != color;
}

class WordmarkSquare extends StatelessWidget {
  const WordmarkSquare({super.key, this.size = 15, this.glow = false});

  final double size;
  final bool glow;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        border: Border.all(color: Palette.amber, width: 1),
        boxShadow: glow
            ? const [
                BoxShadow(
                  color: Color(0x59ffb000),
                  blurRadius: 14,
                  spreadRadius: 4,
                ),
              ]
            : null,
      ),
      padding: EdgeInsets.all(size / 5.5),
      child: const ColoredBox(color: Palette.amber),
    );
  }
}

enum GlyphKind {
  tunnels,
  status,
  log,
  about,
  back,
  chevron,
  scan,
  paste,
  image,
  copy,
  plus,
  close,
  shield,
  call,
  hangup,
  mute,
  speaker,
  edit,
  chat,
  send,
  video,
  flip,
  mic,
  play,
  stop,
}

class Glyph extends StatelessWidget {
  const Glyph(this.kind, {super.key, this.size = 22, this.color = Palette.fg2});

  final GlyphKind kind;
  final double size;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      size: Size.square(size),
      painter: _GlyphPainter(kind, color),
    );
  }
}

class _GlyphPainter extends CustomPainter {
  const _GlyphPainter(this.kind, this.color);

  final GlyphKind kind;
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.scale(size.width / 24);

    final line = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.6
      ..strokeCap = StrokeCap.square
      ..strokeJoin = StrokeJoin.miter;
    final solid = Paint()..color = color;

    void poly(List<double> xy, {bool close = false}) {
      final path = Path()..moveTo(xy[0], xy[1]);
      for (var i = 2; i < xy.length; i += 2) {
        path.lineTo(xy[i], xy[i + 1]);
      }

      if (close) {
        path.close();
      }
      canvas.drawPath(path, line);
    }

    switch (kind) {
      case GlyphKind.tunnels:
        canvas.drawRect(const Rect.fromLTWH(3, 3, 6, 6), line);
        canvas.drawRect(const Rect.fromLTWH(15, 15, 6, 6), line);
        canvas.drawRect(const Rect.fromLTWH(16, 4, 4, 4), solid);
        poly([9, 9, 15, 15]);
        poly([9, 6, 16, 6]);
      case GlyphKind.status:
        poly([2, 13, 7, 13, 10, 5, 14, 19, 17, 11, 22, 11]);
      case GlyphKind.log:
        canvas.drawPath(
          cutPath(const Size(20, 18), cut: 4).shift(const Offset(2, 3)),
          line,
        );
        poly([6, 9, 9, 12, 6, 15]);
        poly([11, 15, 16, 15]);
      case GlyphKind.about:
        poly([
          10,
          2,
          14,
          2,
          14,
          7,
          19,
          7,
          19,
          11,
          14,
          11,
          14,
          22,
          10,
          22,
          10,
          11,
          5,
          11,
          5,
          7,
          10,
          7,
        ], close: true);
      case GlyphKind.back:
        poly([15, 4, 7, 12, 15, 20]);
      case GlyphKind.chevron:
        poly([9, 5, 16, 12, 9, 19]);
      case GlyphKind.scan:
        poly([3, 8, 3, 3, 8, 3]);
        poly([16, 3, 21, 3, 21, 8]);
        poly([21, 16, 21, 21, 16, 21]);
        poly([8, 21, 3, 21, 3, 16]);
        poly([7, 12, 17, 12]);
      case GlyphKind.paste:
        canvas.drawPath(
          cutPath(const Size(14, 18), cut: 4).shift(const Offset(5, 3)),
          line,
        );
        poly([8, 9, 15, 9]);
        poly([8, 13, 15, 13]);
        poly([8, 17, 12, 17]);
      case GlyphKind.image:
        canvas.drawRect(const Rect.fromLTWH(3, 4, 18, 16), line);
        poly([3, 17, 9, 11, 13, 15, 16, 12, 21, 17]);
        canvas.drawRect(const Rect.fromLTWH(15, 7, 3, 3), solid);
      case GlyphKind.copy:
        canvas.drawRect(const Rect.fromLTWH(8, 8, 12, 13), line);
        poly([5, 16, 4, 16, 4, 3, 16, 3, 16, 4]);
      case GlyphKind.plus:
        poly([12, 4, 12, 20]);
        poly([4, 12, 20, 12]);
      case GlyphKind.close:
        poly([5, 5, 19, 19]);
        poly([19, 5, 5, 19]);
      case GlyphKind.edit:
        poly([4, 20, 4, 16, 15, 5, 19, 9, 8, 20], close: true);
        poly([12, 8, 16, 12]);
        poly([12, 20, 20, 20]);
      case GlyphKind.chat:
        poly([3, 4, 21, 4, 21, 16, 11, 16, 6, 20, 6, 16, 3, 16], close: true);
        poly([7, 9, 17, 9]);
        poly([7, 12, 13, 12]);
      case GlyphKind.send:
        poly([3, 4, 21, 12, 3, 20, 6, 12], close: true);
        poly([6, 12, 13, 12]);
      case GlyphKind.shield:
        poly([12, 2, 20, 5, 20, 12, 12, 22, 4, 12, 4, 5], close: true);
        canvas.drawRect(const Rect.fromLTWH(11, 7, 2, 9), solid);
        canvas.drawRect(const Rect.fromLTWH(9, 9, 6, 2), solid);
      case GlyphKind.call:
        poly([
          5,
          3,
          9,
          3,
          11,
          8,
          8.5,
          10.5,
          13.5,
          15.5,
          16,
          13,
          21,
          15,
          21,
          19,
          19,
          21,
          15,
          21,
          3,
          9,
          3,
          5,
        ], close: true);
      case GlyphKind.hangup:
        poly([
          2,
          14,
          5,
          10,
          12,
          8,
          19,
          10,
          22,
          14,
          20,
          16,
          16,
          15,
          16,
          12,
          12,
          11,
          8,
          12,
          8,
          15,
          4,
          16,
        ], close: true);
      case GlyphKind.mute || GlyphKind.mic:
        canvas.drawRect(const Rect.fromLTWH(9, 3, 6, 10), line);
        poly([6, 11, 6, 13, 9, 16, 15, 16, 18, 13, 18, 11]);
        poly([12, 16, 12, 21]);
        poly([8, 21, 16, 21]);
      case GlyphKind.speaker:
        poly([3, 9, 7, 9, 12, 5, 12, 19, 7, 15, 3, 15], close: true);
        poly([15, 9, 17, 12, 15, 15]);
        poly([18, 6, 21, 12, 18, 18]);
      case GlyphKind.video:
        canvas.drawRect(const Rect.fromLTWH(2, 6, 14, 12), line);
        poly([16, 10, 22, 7, 22, 17, 16, 14]);
      case GlyphKind.flip:
        poly([4, 10, 7, 6, 17, 6, 20, 10]);
        poly([20, 14, 17, 18, 7, 18, 4, 14]);
        poly([17, 3, 20, 6, 17, 9]);
        poly([7, 15, 4, 18, 7, 21]);
      case GlyphKind.play:
        canvas.drawPath(
          Path()
            ..moveTo(7, 4)
            ..lineTo(20, 12)
            ..lineTo(7, 20)
            ..close(),
          solid,
        );
      case GlyphKind.stop:
        canvas.drawRect(const Rect.fromLTWH(6, 6, 12, 12), solid);
    }
  }

  @override
  bool shouldRepaint(covariant _GlyphPainter oldDelegate) =>
      oldDelegate.kind != kind || oldDelegate.color != color;
}

class FinderPainter extends CustomPainter {
  const FinderPainter({this.length = 28});

  final double length;

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = Palette.amber
      ..style = PaintingStyle.stroke
      ..strokeWidth = 3
      ..strokeCap = StrokeCap.square;
    final l = math.min(length, size.shortestSide / 3);
    final w = size.width;
    final h = size.height;

    canvas.drawPath(
      Path()
        ..moveTo(0, l)
        ..lineTo(0, 0)
        ..lineTo(l, 0)
        ..moveTo(w - l, 0)
        ..lineTo(w, 0)
        ..lineTo(w, l)
        ..moveTo(w, h - l)
        ..lineTo(w, h)
        ..lineTo(w - l, h)
        ..moveTo(l, h)
        ..lineTo(0, h)
        ..lineTo(0, h - l),
      paint,
    );
  }

  @override
  bool shouldRepaint(covariant FinderPainter oldDelegate) =>
      oldDelegate.length != length;
}
