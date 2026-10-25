import 'package:flutter/widgets.dart';

/// Taken from :root in sanctum.css.
abstract final class Palette {
  static const bg = Color(0xff0b0c0e);
  static const bg2 = Color(0xff111317);
  static const bg3 = Color(0xff16191e);
  static const line = Color(0xff23262c);
  static const line2 = Color(0xff353941);
  static const fg = Color(0xffe8e6e1);
  static const fg2 = Color(0xffb9b7b0);
  static const dim = Color(0xff7e8088);
  static const amber = Color(0xffffb000);
  static const amber2 = Color(0xffffc94d);
  static const amberDim = Color(0xff6b4a00);
  static const good = Color(0xff6ee7a0);
  static const pre = Color(0xffcfcdc6);

  static const amberWash = Color(0x14ffb000);
}

abstract final class Fonts {
  static const head = 'ChakraPetch';
  static const mono = 'JetBrainsMono';
}

abstract final class Metrics {
  static const double edge = 18;

  static const double cut = 10;

  static const double gap = 14;

  static const double tabBar = 62;
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

String hex(int value, {bool prefix = true}) {
  final digits = value.toRadixString(16).padLeft(2, '0');
  if (!prefix) {
    return digits;
  }

  return '0x$digits';
}
