import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:image_picker/image_picker.dart';
import 'package:mobile_scanner/mobile_scanner.dart';

import '../app.dart';
import '../vigil.dart';
import '../theme.dart';
import '../widgets/buttons.dart';
import '../widgets/marks.dart';
import '../widgets/ui.dart';
import 'tunnel.dart';
import 'tunnels.dart';

const _magic = 'vigil ';

class ScanPage extends StatefulWidget {
  const ScanPage({super.key});

  @override
  State<ScanPage> createState() => _ScanPageState();
}

class _ScanPageState extends State<ScanPage> {
  final MobileScannerController _scanner = MobileScannerController(
    formats: const [BarcodeFormat.qrCode],
    detectionSpeed: DetectionSpeed.noDuplicates,
    cameraResolution: const Size(1920, 1080),
    autoZoom: true,
  );

  bool _done = false;
  String? _problem;

  @override
  void dispose() {
    unawaited(_scanner.dispose());
    super.dispose();
  }

  void _found(String? value) {
    if (_done || value == null) {
      return;
    }

    if (!value.startsWith(_magic)) {
      setState(() => _problem = 'That QR code is not a vigil configuration.');
      return;
    }

    _done = true;
    HapticFeedback.mediumImpact();
    unawaited(_scanner.stop());

    replace<void>(ReviewPage(config: value)).ignore();
  }

  Future<void> _fromImage() async {
    final image = await ImagePicker().pickImage(source: ImageSource.gallery);
    if (image == null) {
      return;
    }

    final capture = await _scanner.analyzeImage(image.path);
    final codes = capture?.barcodes ?? const [];

    if (codes.isEmpty) {
      setState(() => _problem = 'No QR code was found in that image.');
      return;
    }

    _found(codes.first.rawValue);
  }

  @override
  Widget build(BuildContext context) {
    final padding = MediaQuery.paddingOf(context);

    return ColoredBox(
      color: const Color(0xff000000),
      child: LayoutBuilder(
        builder: (context, constraints) {
          final side = constraints.maxWidth * 0.72;
          // Only a guide, with a scan window the scanner drops dense codes
          // that stick out of it.
          final window = Rect.fromCenter(
            center: Offset(
              constraints.maxWidth / 2,
              constraints.maxHeight * 0.42,
            ),
            width: side,
            height: side,
          );

          return Stack(
            children: [
              Positioned.fill(
                child: MobileScanner(
                  controller: _scanner,
                  onDetect: (capture) => _found(
                    capture.barcodes.isEmpty
                        ? null
                        : capture.barcodes.first.rawValue,
                  ),
                  errorBuilder: (context, error) => _CameraProblem(error),
                ),
              ),
              Positioned.fill(
                child: IgnorePointer(
                  child: CustomPaint(painter: _ShadePainter(window)),
                ),
              ),
              Positioned.fromRect(
                rect: window,
                child: const IgnorePointer(
                  child: CustomPaint(painter: FinderPainter()),
                ),
              ),
              Positioned(
                top: padding.top,
                left: 0,
                right: 0,
                child: SizedBox(
                  height: 56,
                  child: Row(
                    children: [
                      const SizedBox(width: Metrics.edge - 12),
                      GlyphBtn(
                        GlyphKind.close,
                        label: 'Close',
                        onTap: () => Navigator.of(context).maybePop(),
                      ),
                      const SizedBox(width: 2),
                      Text('Scan QR code', style: Styles.barTitle),
                    ],
                  ),
                ),
              ),
              Positioned(
                top: window.bottom + 22,
                left: Metrics.edge,
                right: Metrics.edge,
                child: Text(
                  'POINT THE CAMERA AT THE QR CODE YOU WERE GIVEN',
                  style: Styles.rubric.copyWith(color: Palette.fg2),
                  textAlign: TextAlign.center,
                ),
              ),
              Positioned(
                left: Metrics.edge,
                right: Metrics.edge,
                bottom: padding.bottom + 18,
                child: Column(
                  children: [
                    if (_problem != null) ...[
                      Notice(kind: NoticeKind.warn, text: _problem!),
                      const SizedBox(height: 12),
                    ],
                    BtnPair(
                      first: Btn(
                        label: 'Image',
                        glyph: GlyphKind.image,
                        kind: BtnKind.secondary,
                        expand: true,
                        onTap: _fromImage,
                      ),
                      second: Btn(
                        label: 'Paste',
                        glyph: GlyphKind.paste,
                        kind: BtnKind.secondary,
                        expand: true,
                        onTap: () => replace<void>(const PastePage()).ignore(),
                      ),
                    ),
                  ],
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}

class _ShadePainter extends CustomPainter {
  const _ShadePainter(this.window);

  final Rect window;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawPath(
      Path.combine(
        PathOperation.difference,
        Path()..addRect(Offset.zero & size),
        Path()..addRect(window),
      ),
      Paint()..color = const Color(0xb30b0c0e),
    );
  }

  @override
  bool shouldRepaint(covariant _ShadePainter oldDelegate) =>
      oldDelegate.window != window;
}

class _CameraProblem extends StatelessWidget {
  const _CameraProblem(this.error);

  final MobileScannerException error;

  @override
  Widget build(BuildContext context) {
    final text = switch (error.errorCode) {
      MobileScannerErrorCode.permissionDenied =>
        'Camera access was denied, allow it in the system settings or '
            'import the configuration another way.',
      MobileScannerErrorCode.unsupported =>
        'This device has no camera that can scan QR codes.',
      _ => 'The camera could not be started.',
    };

    return ColoredBox(
      color: Palette.bg,
      child: Center(
        child: Padding(
          padding: const EdgeInsets.all(40),
          child: Text(text, style: Styles.body, textAlign: TextAlign.center),
        ),
      ),
    );
  }
}

class PastePage extends StatefulWidget {
  const PastePage({super.key});

  @override
  State<PastePage> createState() => _PastePageState();
}

class _PastePageState extends State<PastePage> {
  final TextEditingController _text = TextEditingController();

  @override
  void initState() {
    super.initState();
    _text.addListener(() => setState(() {}));
  }

  @override
  void dispose() {
    _text.dispose();
    super.dispose();
  }

  Future<void> _clipboard() async {
    final data = await Clipboard.getData(Clipboard.kTextPlain);
    if (data?.text != null) {
      _text.text = data!.text!;
    }
  }

  void _check() {
    final config = _text.text.trim();
    if (config.isEmpty) {
      return;
    }

    push<void>(ReviewPage(config: '$config\n')).ignore();
  }

  @override
  Widget build(BuildContext context) {
    return PushedPage(
      rubric: 'Import',
      title: 'Paste a configuration',
      bottom: BtnPair(
        first: Btn(
          label: 'Clipboard',
          glyph: GlyphKind.paste,
          kind: BtnKind.secondary,
          expand: true,
          onTap: _clipboard,
        ),
        second: Btn(
          label: 'Check',
          expand: true,
          onTap: _text.text.trim().isEmpty ? null : _check,
        ),
      ),
      children: [
        const Text(
          'Paste the text that is inside of the QR code, it starts with '
          'the line "vigil 1".',
          style: Styles.body,
        ),
        Pre(
          child: TextField(
            controller: _text,
            minLines: 12,
            maxLines: 20,
            autocorrect: false,
            enableSuggestions: false,
            keyboardType: TextInputType.multiline,
            style: Styles.pre,
            cursorColor: Palette.amber,
            decoration: InputDecoration.collapsed(
              hintText: 'vigil 1\ninstance ...',
              hintStyle: Styles.pre.copyWith(color: Palette.dim),
            ),
          ),
        ),
      ],
    );
  }
}

class ReviewPage extends StatefulWidget {
  const ReviewPage({super.key, required this.config});

  final String config;

  @override
  State<ReviewPage> createState() => _ReviewPageState();
}

class _ReviewPageState extends State<ReviewPage> {
  late final Future<Summary> _summary = Vigil.instance.check(widget.config);
  bool _busy = false;

  Future<void> _store() async {
    setState(() => _busy = true);

    try {
      final tunnel = await Vigil.instance.store(widget.config);
      navigatorKey.currentState!.popUntil((route) => route.isFirst);
      if (tunnel.summary.voice) {
        currentTab.value = NavTab.gospel;
      } else {
        currentTab.value = NavTab.tunnels;
      }
      push(TunnelPage(tunnel: tunnel)).ignore();
      notify(
        tunnel.summary.voice
            ? 'Gospel configuration imported.'
            : 'Tunnel imported.',
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
    return FutureBuilder<Summary>(
      future: _summary,
      builder: (context, snapshot) {
        if (snapshot.hasError) {
          return PushedPage(
            rubric: 'Review',
            title: 'Not imported',
            children: [
              Notice(
                kind: NoticeKind.warn,
                text: 'This configuration was refused: ${snapshot.error}.',
              ),
            ],
          );
        }

        if (!snapshot.hasData) {
          return const PushedPage(
            rubric: 'Review',
            title: 'Checking…',
            children: [],
          );
        }

        final s = snapshot.data!;

        return PushedPage(
          rubric: 'Review',
          title: s.instance,
          bottom: BtnPair(
            first: Btn(
              label: 'Cancel',
              kind: BtnKind.secondary,
              expand: true,
              onTap: () => goTab(NavTab.tunnels),
            ),
            second: Btn(
              label: 'Import',
              expand: true,
              busy: _busy,
              onTap: _store,
            ),
          ),
          children: [
            Panel(
              accent: true,
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Tag(modeTag(s), accent: true),
                  const SizedBox(height: 12),
                  Text(s.instance, style: Styles.state),
                  const SizedBox(height: 4),
                  Text('via ${s.endpoint}', style: Styles.detail),
                ],
              ),
            ),
            const GroupLabel('Settings', index: 1),
            KeyList(rows: s.rows),
            const Notice(
              text:
                  'This configuration carries the keys for the tunnel. '
                  'They are stored encrypted on this device and are never '
                  'shown again.',
            ),
          ],
        );
      },
    );
  }
}
