import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/widgets.dart';

import '../theme.dart';
import '../widgets/buttons.dart';
import '../widgets/marks.dart';

class AvatarCrop extends StatefulWidget {
  const AvatarCrop({super.key, required this.image});

  final ui.Image image;

  @override
  State<AvatarCrop> createState() => _AvatarCropState();
}

class _AvatarCropState extends State<AvatarCrop> {
  static const _zoomMax = 8.0;
  static const _side = 256;

  double _view = 0;
  double _zoom = 1;
  double _zoomStart = 1;
  Offset _offset = Offset.zero;
  Offset _focus = Offset.zero;
  bool _busy = false;

  double get _width => widget.image.width.toDouble();

  double get _height => widget.image.height.toDouble();

  double get _scale => _view / math.min(_width, _height) * _zoom;

  void _layout(double view) {
    if (view == _view) {
      return;
    }

    _view = view;
    _zoom = 1;
    _offset = Offset(
      (view - _width * _scale) / 2,
      (view - _height * _scale) / 2,
    );
  }

  Offset _clamp(Offset offset) {
    final scale = _scale;

    return Offset(
      offset.dx.clamp(_view - _width * scale, 0),
      offset.dy.clamp(_view - _height * scale, 0),
    );
  }

  void _start(ScaleStartDetails details) {
    _zoomStart = _zoom;
    _focus = (details.localFocalPoint - _offset) / _scale;
  }

  void _update(ScaleUpdateDetails details) {
    setState(() {
      _zoom = (_zoomStart * details.scale).clamp(1, _zoomMax);
      _offset = _clamp(details.localFocalPoint - _focus * _scale);
    });
  }

  Future<void> _done() async {
    setState(() => _busy = true);

    final scale = _scale;
    final from = Rect.fromLTWH(
      -_offset.dx / scale,
      -_offset.dy / scale,
      _view / scale,
      _view / scale,
    );
    final side = math.max(1, math.min(_side, from.width.round()));
    final recorder = ui.PictureRecorder();

    Canvas(recorder).drawImageRect(
      widget.image,
      from,
      Rect.fromLTWH(0, 0, side.toDouble(), side.toDouble()),
      Paint()..filterQuality = FilterQuality.high,
    );

    final picture = recorder.endRecording();
    final cut = await picture.toImage(side, side);
    final data = await cut.toByteData(format: ui.ImageByteFormat.png);

    picture.dispose();
    cut.dispose();

    if (!mounted) {
      return;
    }

    Navigator.of(context).pop(data?.buffer.asUint8List());
  }

  @override
  Widget build(BuildContext context) {
    return ColoredBox(
      color: Palette.bg,
      child: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(Metrics.edge),
          child: Column(
            children: [
              Row(
                children: [
                  GlyphBtn(
                    GlyphKind.close,
                    label: 'Cancel',
                    onTap: () => Navigator.of(context).maybePop(),
                  ),
                  const SizedBox(width: 8),
                  Text('Move and zoom', style: Styles.barTitle),
                ],
              ),
              const SizedBox(height: 16),
              Expanded(
                child: LayoutBuilder(
                  builder: (context, box) {
                    _layout(math.min(box.maxWidth, box.maxHeight));

                    final scale = _scale;

                    return Center(
                      child: GestureDetector(
                        onScaleStart: _start,
                        onScaleUpdate: _update,
                        child: SizedBox.square(
                          dimension: _view,
                          child: ClipRect(
                            child: Stack(
                              children: [
                                Positioned(
                                  left: _offset.dx,
                                  top: _offset.dy,
                                  width: _width * scale,
                                  height: _height * scale,
                                  child: RawImage(
                                    image: widget.image,
                                    fit: BoxFit.fill,
                                    filterQuality: FilterQuality.medium,
                                  ),
                                ),
                                const Positioned.fill(
                                  child: IgnorePointer(
                                    child: CustomPaint(painter: _Mask()),
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ),
                      ),
                    );
                  },
                ),
              ),
              const SizedBox(height: 16),
              Btn(
                label: 'Use picture',
                expand: true,
                busy: _busy,
                onTap: _done,
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _Mask extends CustomPainter {
  const _Mask();

  @override
  void paint(Canvas canvas, Size size) {
    final area = Offset.zero & size;
    final hole = Path()
      ..fillType = PathFillType.evenOdd
      ..addRect(area)
      ..addOval(area);

    canvas.drawPath(hole, Paint()..color = const Color(0xb3000000));
    canvas.drawOval(
      area.deflate(0.75),
      Paint()
        ..color = Palette.amber
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.5,
    );
  }

  @override
  bool shouldRepaint(_Mask oldDelegate) => false;
}
