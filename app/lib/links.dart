class Link {
  const Link(this.start, this.end);

  final int start;
  final int end;
}

class Canonical {
  const Canonical({
    required this.uri,
    required this.shown,
    required this.user,
    required this.address,
  });

  final Uri uri;
  final String shown;
  final bool user;
  final bool address;

  String get host => uri.host;
  bool get plain => uri.scheme == 'http';
  bool get foreign => shown != uri.host;
}

final _link = RegExp(
  r'''(?:https?://|www\.)[^\s<>"'`]+''',
  caseSensitive: false,
);
final _ipv4 = RegExp(r'^\d{1,3}(?:\.\d{1,3}){3}$');

const _trailing = '.,;:!?';

List<Link> links(String text) {
  final found = <Link>[];

  for (final match in _link.allMatches(text)) {
    var end = match.end;

    while (end > match.start) {
      final c = text[end - 1];

      if (_trailing.contains(c)) {
        end--;
        continue;
      }

      if (c == ')' && !text.substring(match.start, end).contains('(')) {
        end--;
        continue;
      }

      break;
    }

    if (canonical(text.substring(match.start, end)) != null) {
      found.add(Link(match.start, end));
    }
  }

  return found;
}

Canonical? canonical(String raw) {
  final full = raw.toLowerCase().startsWith('www.') ? 'https://$raw' : raw;
  final uri = Uri.tryParse(full);

  if (uri == null) {
    return null;
  }

  if (uri.scheme != 'http' && uri.scheme != 'https') {
    return null;
  }

  if (uri.host.isEmpty) {
    return null;
  }

  final String shown;

  try {
    shown = Uri.decodeComponent(uri.host).toLowerCase();
  } on ArgumentError {
    return null;
  }

  final host = shown.split('.').map(_label).join('.');

  return Canonical(
    uri: uri.replace(host: host),
    shown: shown,
    user: uri.userInfo.isNotEmpty,
    address: host.contains(':') || _ipv4.hasMatch(host),
  );
}

String _label(String label) {
  if (label.runes.every((c) => c < 0x80)) {
    return label;
  }

  return 'xn--${_punycode(label)}';
}

const _base = 36;
const _tmin = 1;
const _tmax = 26;
const _skew = 38;
const _damp = 700;

String _punycode(String input) {
  final points = input.runes.toList();
  final out = StringBuffer();

  for (final c in points) {
    if (c < 0x80) {
      out.writeCharCode(c);
    }
  }

  final basic = out.length;
  var handled = basic;
  var n = 0x80;
  var delta = 0;
  var bias = 72;

  if (basic > 0) {
    out.write('-');
  }

  while (handled < points.length) {
    var m = 0x110000;

    for (final c in points) {
      if (c >= n && c < m) {
        m = c;
      }
    }

    delta += (m - n) * (handled + 1);
    n = m;

    for (final c in points) {
      if (c < n) {
        delta++;
      }

      if (c != n) {
        continue;
      }

      var q = delta;

      for (var k = _base; ; k += _base) {
        var t = k - bias;

        if (k <= bias) {
          t = _tmin;
        } else if (k >= bias + _tmax) {
          t = _tmax;
        }

        if (q < t) {
          break;
        }

        out.writeCharCode(_digit(t + (q - t) % (_base - t)));
        q = (q - t) ~/ (_base - t);
      }

      out.writeCharCode(_digit(q));
      bias = _adapt(delta, handled + 1, handled == basic);
      delta = 0;
      handled++;
    }

    delta++;
    n++;
  }

  return out.toString();
}

int _digit(int d) {
  if (d < 26) {
    return 0x61 + d;
  }

  return 0x16 + d;
}

int _adapt(int delta, int points, bool first) {
  var d = first ? delta ~/ _damp : delta ~/ 2;
  var k = 0;

  d += d ~/ points;

  while (d > ((_base - _tmin) * _tmax) ~/ 2) {
    d ~/= _base - _tmin;
    k += _base;
  }

  return k + (_base - _tmin + 1) * d ~/ (d + _skew);
}
