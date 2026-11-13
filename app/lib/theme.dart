import 'package:flutter/widgets.dart';

abstract final class Palette {
  static const bg = Color(0xff15120f);
  static const bg2 = Color(0xff1e1a16);
  static const bg3 = Color(0xff26211b);
  static const well = Color(0xff100e0b);
  static const line = Color(0xff2a241e);
  static const line2 = Color(0xff3a3229);
  static const fg = Color(0xffefe7da);
  static const fg2 = Color(0xffbfb3a2);
  static const dim = Color(0xff8c8173);
  static const amber = Color(0xffe8a33d);
  static const amber2 = Color(0xfff2bb5e);
  static const amberDim = Color(0xff5a4220);
  static const good = Color(0xff8fbf8a);
  static const pre = Color(0xffd9cdb9);
  static const mine = Color(0xffd6b98d);

  static const amberWash = Color(0x1fe8a33d);
  static const veil = Color(0xcc15120f);
}

abstract final class Fonts {
  static const head = 'ChakraPetch';
  static const mono = 'JetBrainsMono';
}

abstract final class Metrics {
  static const double edge = 20;

  static const double radius = 16;

  static const double gap = 12;

  static const double tabBar = 64;
}

/// JetBrains Mono only ships regular and bold.
const FontWeight _mono500 = FontWeight.w400;
const FontWeight _mono600 = FontWeight.w700;

double _em(double size, double em) => size * em;

abstract final class Styles {
  static const body = TextStyle(fontSize: 15, height: 1.55, color: Palette.fg2);

  static final wordmark = TextStyle(
    fontFamily: Fonts.head,
    fontWeight: FontWeight.w600,
    fontSize: 15,
    height: 1,
    letterSpacing: _em(15, 0.32),
    color: Palette.fg,
  );

  static final title = TextStyle(
    fontFamily: Fonts.head,
    fontWeight: FontWeight.w500,
    fontSize: 32,
    height: 1.1,
    letterSpacing: _em(32, -0.005),
    color: Palette.fg,
  );

  static final barTitle = TextStyle(
    fontFamily: Fonts.head,
    fontWeight: FontWeight.w600,
    fontSize: 18,
    height: 1.2,
    letterSpacing: _em(18, 0.02),
    color: Palette.fg,
  );

  static final rubric = TextStyle(
    fontFamily: Fonts.mono,
    fontWeight: _mono500,
    fontSize: 11,
    height: 1.3,
    letterSpacing: _em(11, 0.14),
    color: Palette.dim,
  );

  static final state = TextStyle(
    fontFamily: Fonts.head,
    fontWeight: FontWeight.w500,
    fontSize: 28,
    height: 1.1,
    color: Palette.fg,
  );

  static final cardTitle = TextStyle(
    fontFamily: Fonts.head,
    fontWeight: FontWeight.w600,
    fontSize: 18,
    height: 1.2,
    letterSpacing: _em(18, 0.02),
    color: Palette.fg,
  );

  static const detail = TextStyle(
    fontFamily: Fonts.mono,
    fontSize: 12,
    height: 1.4,
    color: Palette.dim,
  );

  static final figure = TextStyle(
    fontFamily: Fonts.head,
    fontWeight: FontWeight.w500,
    fontSize: 22,
    height: 1.1,
    color: Palette.fg,
  );

  static final btn = TextStyle(
    fontFamily: Fonts.mono,
    fontWeight: _mono600,
    fontSize: 13.5,
    height: 1,
    letterSpacing: _em(13.5, 0.06),
  );

  static final tab = TextStyle(
    fontFamily: Fonts.mono,
    fontWeight: _mono500,
    fontSize: 10,
    height: 1,
    letterSpacing: _em(10, 0.1),
  );

  static final key = TextStyle(
    fontFamily: Fonts.mono,
    fontWeight: _mono500,
    fontSize: 11.5,
    height: 1.4,
    letterSpacing: _em(11.5, 0.1),
    color: Palette.dim,
  );

  static const value = TextStyle(
    fontFamily: Fonts.mono,
    fontSize: 13,
    height: 1.45,
    color: Palette.fg,
  );

  static const pre = TextStyle(
    fontFamily: Fonts.mono,
    fontSize: 12.5,
    height: 1.6,
    color: Palette.pre,
  );

  static const notice = TextStyle(
    fontSize: 14,
    height: 1.5,
    color: Palette.fg2,
  );
}

String roman(int value) {
  const marks = [(10, 'X'), (9, 'IX'), (5, 'V'), (4, 'IV'), (1, 'I')];
  final out = StringBuffer();
  var left = value;

  for (final (worth, mark) in marks) {
    while (left >= worth) {
      out.write(mark);
      left -= worth;
    }
  }

  return out.toString();
}
