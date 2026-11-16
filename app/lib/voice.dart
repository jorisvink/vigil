import 'dart:typed_data';

const _adtsRates = [
  96000,
  88200,
  64000,
  48000,
  44100,
  32000,
  24000,
  22050,
  16000,
  12000,
  11025,
  8000,
  7350,
];

(int, List<double>?) voiceShape(Uint8List? bytes, int count) {
  final sizes = <int>[];
  var rate = 0;
  var off = 0;

  while (bytes != null && off + 7 <= bytes.length) {
    if (bytes[off] != 0xff || (bytes[off + 1] & 0xf6) != 0xf0) {
      break;
    }

    final index = (bytes[off + 2] >> 2) & 0x0f;
    if (index >= _adtsRates.length) {
      break;
    }

    final len =
        ((bytes[off + 3] & 0x03) << 11) |
        (bytes[off + 4] << 3) |
        (bytes[off + 5] >> 5);
    if (len <= 7) {
      break;
    }

    rate = _adtsRates[index];
    sizes.add(len);
    off += len;
  }

  if (sizes.isEmpty) {
    return (0, null);
  }

  final ms = sizes.length * 1024 * 1000 ~/ rate;
  final bars = <double>[];

  for (var i = 0; i < count; i++) {
    final from = i * sizes.length ~/ count;
    final to = (i + 1) * sizes.length ~/ count;
    var sum = 0;

    for (var j = from; j < to; j++) {
      sum += sizes[j];
    }

    bars.add(to > from ? sum / (to - from) : 0);
  }

  final low = bars.reduce((a, b) => a < b ? a : b);
  final high = bars.reduce((a, b) => a > b ? a : b);

  if (high - low < high * 0.08) {
    return (ms, null);
  }

  return (ms, [for (final b in bars) 0.15 + 0.85 * (b - low) / (high - low)]);
}
