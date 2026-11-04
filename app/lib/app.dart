import 'dart:async';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:url_launcher/url_launcher.dart';

import 'pages/about.dart';
import 'pages/call.dart';
import 'pages/chats.dart';
import 'pages/gospel.dart';
import 'pages/import.dart';
import 'pages/log.dart';
import 'pages/status.dart';
import 'pages/tunnels.dart';
import 'vigil.dart';
import 'theme.dart';
import 'widgets/background.dart';
import 'widgets/buttons.dart';
import 'widgets/marks.dart';
import 'widgets/ui.dart';

enum NavTab { tunnels, status, gospel, log, about }

const _tabs = [
  ('Tunnels', GlyphKind.tunnels),
  ('Status', GlyphKind.status),
  ('Gospel', GlyphKind.chat),
  ('Log', GlyphKind.log),
  ('About', GlyphKind.about),
];

const lockdownNotice =
    'You currently have "Always-on VPN" and "Block connections without VPN" '
    'turned on. This results in Android blocking all traffic that is not '
    'ment for any of your configured routes. Turn off '
    '"Block connections without VPN" if you want to bring back normal traffic.';

final GlobalKey<NavigatorState> navigatorKey = GlobalKey<NavigatorState>();
final ValueNotifier<NavTab> currentTab = ValueNotifier(NavTab.tunnels);

class VigilApp extends StatelessWidget {
  const VigilApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Vigil',
      navigatorKey: navigatorKey,
      debugShowCheckedModeBanner: false,
      theme: _theme(),
      builder: (context, child) => Material(
        type: MaterialType.transparency,
        child: DefaultTextStyle(
          style: Styles.body,
          child: Stack(
            children: [
              child!,
              const Positioned.fill(child: LinkOverlay()),
            ],
          ),
        ),
      ),
      home: const Shell(),
    );
  }

  ThemeData _theme() {
    return ThemeData(
      brightness: Brightness.dark,
      useMaterial3: true,
      splashFactory: NoSplash.splashFactory,
      highlightColor: const Color(0x00000000),
      scaffoldBackgroundColor: Palette.bg,
      canvasColor: Palette.bg,
      colorScheme: const ColorScheme.dark(
        primary: Palette.amber,
        onPrimary: Palette.bg,
        secondary: Palette.amber2,
        surface: Palette.bg,
        onSurface: Palette.fg2,
        error: Palette.amber,
      ),
      textSelectionTheme: const TextSelectionThemeData(
        cursorColor: Palette.amber,
        selectionColor: Color(0x99ffb000),
        selectionHandleColor: Palette.amber,
      ),
      textTheme: const TextTheme(bodyMedium: Styles.body),
    );
  }
}

class Shell extends StatefulWidget {
  const Shell({super.key});

  @override
  State<Shell> createState() => _ShellState();
}

class _ShellState extends State<Shell> with WidgetsBindingObserver {
  StreamSubscription<List<Tunnel>>? _tunnels;
  StreamSubscription<Status>? _calls;
  Chats? _opened;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    Vigil.instance.start();
    _handoff();

    _tunnels = Vigil.instance.tunnelUpdates.listen((tunnels) {
      if (!_configured(tunnels) && currentTab.value == NavTab.status) {
        currentTab.value = NavTab.tunnels;
      }
    });

    _calls = Vigil.instance.statuses.listen((s) {
      if (s.phone != null) {
        showCall();
      }

      final peer = s.chats.open;
      if (peer != null && s.phone == null && !identical(s.chats, _opened)) {
        _opened = s.chats;
        goTab(NavTab.gospel);

        if (peer != 'gospel') {
          push(ChatPage(peer: peer), opaque: false);
        }
      }
    });
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _tunnels?.cancel();
    _calls?.cancel();
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      _handoff();
    }
  }

  Future<void> _handoff() async {
    final String? config;

    try {
      config = await Vigil.instance.handoff();
    } on VigilError {
      return;
    }

    if (config != null && mounted) {
      goTab(NavTab.tunnels);
      push(ReviewPage(config: config)).ignore();
    }
  }

  @override
  Widget build(BuildContext context) {
    return DotGrid(
      child: ValueListenableBuilder<NavTab>(
        valueListenable: currentTab,
        builder: (context, tab, _) => Stack(
          children: [
            Positioned.fill(
              child: IndexedStack(
                index: tab.index,
                children: [
                  for (final t in NavTab.values)
                    TickerMode(
                      enabled: t == tab,
                      child: switch (t) {
                        NavTab.tunnels => const TunnelsPage(),
                        NavTab.status => const StatusPage(),
                        NavTab.gospel => const GospelPage(),
                        NavTab.log => const LogPage(),
                        NavTab.about => const AboutPage(),
                      },
                    ),
                ],
              ),
            ),
            Positioned(
              left: 0,
              right: 0,
              bottom: 0,
              height: tabBarSpace(context),
              child: const IgnorePointer(
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    gradient: LinearGradient(
                      begin: Alignment.topCenter,
                      end: Alignment.bottomCenter,
                      colors: [Color(0x000b0c0e), Palette.bg],
                      stops: [0, 0.45],
                    ),
                  ),
                ),
              ),
            ),
            Positioned(
              left: 12,
              right: 12,
              bottom: MediaQuery.paddingOf(context).bottom + 12,
              child: StreamBuilder<List<Tunnel>>(
                stream: Vigil.instance.tunnelUpdates,
                initialData: Vigil.instance.tunnels,
                builder: (context, tunnels) => StreamBuilder<Status>(
                  stream: Vigil.instance.statuses,
                  initialData: Vigil.instance.status,
                  builder: (context, status) => _TabBar(
                    current: tab,
                    unseen: status.data!.chats.unseen,
                    status: _configured(tunnels.data!),
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _TabBar extends StatelessWidget {
  const _TabBar({
    required this.current,
    required this.unseen,
    required this.status,
  });

  final NavTab current;
  final int unseen;
  final bool status;

  @override
  Widget build(BuildContext context) {
    return Container(
      decoration: const BoxDecoration(
        boxShadow: [
          BoxShadow(color: Color(0xcc000000), blurRadius: 24, spreadRadius: 4),
        ],
      ),
      child: CustomPaint(
        painter: const CutPainter(fill: Palette.bg2, stroke: Palette.line2),
        child: SizedBox(
          height: Metrics.tabBar,
          child: Row(
            children: [
              for (final tab in NavTab.values)
                if (tab != NavTab.status || status)
                  Expanded(
                    child: _TabItem(
                      label: _tabs[tab.index].$1,
                      glyph: _tabs[tab.index].$2,
                      active: tab == current,
                      mark: tab == NavTab.gospel && unseen > 0,
                      onTap: () => goTab(tab),
                    ),
                  ),
            ],
          ),
        ),
      ),
    );
  }
}

class _TabItem extends StatelessWidget {
  const _TabItem({
    required this.label,
    required this.glyph,
    required this.active,
    required this.mark,
    required this.onTap,
  });

  final String label;
  final GlyphKind glyph;
  final bool active;
  final bool mark;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Pressable(
      onTap: onTap,
      label: label,
      builder: (context, pressed) {
        final color = active || pressed ? Palette.amber : Palette.dim;

        return Stack(
          children: [
            Center(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Stack(
                    clipBehavior: Clip.none,
                    children: [
                      Glyph(glyph, size: 22, color: color),
                      if (mark)
                        Positioned(
                          top: -2,
                          right: -5,
                          child: Container(
                            width: 7,
                            height: 7,
                            color: Palette.amber,
                          ),
                        ),
                    ],
                  ),
                  const SizedBox(height: 7),
                  Text(
                    label.toUpperCase(),
                    style: Styles.tab.copyWith(
                      color: active ? Palette.fg : color,
                    ),
                  ),
                ],
              ),
            ),
            if (active)
              Positioned(
                top: 0,
                left: 0,
                right: 0,
                child: Center(
                  child: Container(width: 26, height: 2, color: Palette.amber),
                ),
              ),
          ],
        );
      },
    );
  }
}

bool _configured(List<Tunnel> tunnels) => tunnels.any((t) => !t.summary.voice);

void goTab(NavTab tab) {
  navigatorKey.currentState?.popUntil((route) => route.isFirst);
  currentTab.value = tab;
}

Route<T> route<T>(Widget page, {bool opaque = true}) {
  return PageRouteBuilder<T>(
    opaque: opaque,
    pageBuilder: (context, _, _) => page,
    transitionDuration: const Duration(milliseconds: 220),
    reverseTransitionDuration: const Duration(milliseconds: 160),
    transitionsBuilder: (context, animation, _, child) {
      final curved = CurvedAnimation(
        parent: animation,
        curve: Curves.easeOutCubic,
        reverseCurve: Curves.easeInCubic,
      );

      return FadeTransition(
        opacity: curved,
        child: SlideTransition(
          position: Tween(
            begin: const Offset(0.06, 0),
            end: Offset.zero,
          ).animate(curved),
          child: child,
        ),
      );
    },
  );
}

Future<T?> push<T>(Widget page, {bool opaque = true}) {
  return navigatorKey.currentState!.push<T>(route<T>(page, opaque: opaque));
}

class SwipeBack extends StatefulWidget {
  const SwipeBack({super.key, required this.child});

  final Widget child;

  @override
  State<SwipeBack> createState() => _SwipeBackState();
}

class _SwipeBackState extends State<SwipeBack>
    with SingleTickerProviderStateMixin {
  static const _edge = 24.0;
  static const _fling = 700.0;

  late final AnimationController _slide = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 200),
  );

  double _width = 1;
  ModalRoute<Object?>? _route;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();

    final route = ModalRoute.of(context);

    if (route != _route) {
      _route?.animation?.removeStatusListener(_settled);
      _route = route;
      _route?.animation?.addStatusListener(_settled);
    }
  }

  @override
  void dispose() {
    _route?.animation?.removeStatusListener(_settled);
    _slide.dispose();
    super.dispose();
  }

  void _settled(AnimationStatus status) {
    if (status.isCompleted) {
      _cover(true);
    }
  }

  void _cover(bool on) {
    final entries = _route?.overlayEntries ?? const <OverlayEntry>[];

    if (entries.isNotEmpty) {
      entries.first.opaque = on;
    }
  }

  Future<void> _back() async {
    await _slide.animateBack(0, curve: Curves.easeOutCubic);

    if (mounted) {
      _cover(true);
    }
  }

  void _drag(DragUpdateDetails details) {
    _slide.value += details.delta.dx / _width;
  }

  Future<void> _release(DragEndDetails details) async {
    final speed = details.velocity.pixelsPerSecond.dx;

    if (_slide.value < 0.33 && speed < _fling) {
      await _back();
      return;
    }

    await _slide.animateTo(1, curve: Curves.easeOutCubic);

    if (!mounted) {
      return;
    }

    if (!await Navigator.of(context).maybePop()) {
      _slide.value = 0;
      _cover(true);
    }
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, box) {
        _width = box.maxWidth;

        return Stack(
          children: [
            AnimatedBuilder(
              animation: _slide,
              builder: (context, child) => Transform.translate(
                offset: Offset(_slide.value * _width, 0),
                child: child,
              ),
              child: widget.child,
            ),
            Positioned(
              top: 0,
              left: 0,
              bottom: 0,
              width: _edge,
              child: GestureDetector(
                behavior: HitTestBehavior.translucent,
                onHorizontalDragStart: (_) => _cover(false),
                onHorizontalDragUpdate: _drag,
                onHorizontalDragEnd: _release,
                onHorizontalDragCancel: _back,
              ),
            ),
          ],
        );
      },
    );
  }
}

Future<T?> replace<T>(Widget page) {
  return navigatorKey.currentState!.pushReplacement<T, Object?>(route<T>(page));
}

void open(String url) {
  unawaited(launchUrl(Uri.parse(url), mode: LaunchMode.externalApplication));
}

OverlayEntry? _toast;

void notify(String message, {bool warn = true}) {
  final overlay = navigatorKey.currentState?.overlay;
  if (overlay == null) {
    return;
  }

  _toast?.remove();

  late final OverlayEntry entry;

  entry = OverlayEntry(
    builder: (context) => Positioned(
      left: Metrics.edge,
      right: Metrics.edge,
      bottom: tabBarSpace(context),
      child: IgnorePointer(
        child: TweenAnimationBuilder<double>(
          tween: Tween(begin: 0, end: 1),
          duration: const Duration(milliseconds: 180),
          builder: (context, t, child) => Opacity(
            opacity: t,
            child: Transform.translate(
              offset: Offset(0, (1 - t) * 12),
              child: child,
            ),
          ),
          child: Material(
            type: MaterialType.transparency,
            child: Container(
              decoration: const BoxDecoration(
                boxShadow: [
                  BoxShadow(color: Color(0xcc000000), blurRadius: 20),
                ],
              ),
              child: Notice(
                text: message,
                kind: warn ? NoticeKind.warn : NoticeKind.good,
              ),
            ),
          ),
        ),
      ),
    ),
  );

  _toast = entry;
  overlay.insert(entry);

  Timer(const Duration(seconds: 4), () {
    if (_toast == entry) {
      _toast = null;
    }

    if (entry.mounted) {
      entry.remove();
    }
  });
}

TextStyle peerStyle(TextStyle style, bool picked) {
  if (!picked) {
    return style;
  }

  return style.copyWith(fontStyle: FontStyle.italic);
}

class PeerName extends StatelessWidget {
  const PeerName(
    this.peer, {
    super.key,
    required this.style,
    this.name,
    this.picked,
    this.shrink,
  });

  final String peer;
  final TextStyle style;
  final String? name;
  final bool? picked;
  final double? shrink;

  @override
  Widget build(BuildContext context) {
    final vigil = Vigil.instance;
    final own = peer == selfPeer;
    final shown = name ?? (own ? vigil.selfName : vigil.name(peer));
    final sealed = own
        ? vigil.selfSigned
        : vigil.authenticated(peer) && shown == vigil.name(peer);
    final styled = peerStyle(
      style,
      picked ?? (own ? vigil.selfPicked : vigil.picked(peer)),
    );
    final smallest = shrink;

    if (smallest == null) {
      return _row(shown, styled, sealed);
    }

    return LayoutBuilder(
      builder: (context, box) {
        final base = styled.fontSize ?? 14;
        final painter = TextPainter(
          text: TextSpan(text: shown, style: styled),
          textDirection: TextDirection.ltr,
          textScaler: MediaQuery.textScalerOf(context),
          maxLines: 1,
        )..layout();
        final room = box.maxWidth - (sealed ? base * 0.95 * 1.4 : 0);
        final width = painter.width;

        painter.dispose();

        if (width <= room) {
          return _row(shown, styled, sealed);
        }

        final font = math.max(smallest, base * room / width);
        final fitted = styled.copyWith(
          fontSize: font,
          letterSpacing: (styled.letterSpacing ?? 0) * font / base,
        );

        return _row(shown, fitted, sealed);
      },
    );
  }

  Widget _row(String shown, TextStyle styled, bool sealed) {
    final size = (styled.fontSize ?? 14) * 0.95;

    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Flexible(
          child: Text(
            shown,
            style: styled,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
          ),
        ),
        if (sealed) ...[
          SizedBox(width: size * 0.4),
          Semantics(
            label: 'Signed by the flock',
            child: Glyph(GlyphKind.seal, size: size, color: Palette.good),
          ),
        ],
      ],
    );
  }
}

class Avatar extends StatelessWidget {
  const Avatar(
    this.peer, {
    super.key,
    this.size = 36,
    this.margin = const EdgeInsets.only(right: 12),
    this.zoom = false,
  });

  final String peer;
  final double size;
  final EdgeInsets margin;
  final bool zoom;

  @override
  Widget build(BuildContext context) {
    final ratio = MediaQuery.devicePixelRatioOf(context);

    return FutureBuilder<Uint8List?>(
      future: Vigil.instance.avatar(peer),
      builder: (context, snapshot) {
        final data = snapshot.data;

        if (data == null) {
          return const SizedBox.shrink();
        }

        final portrait = ClipPath(
          clipper: const PortraitClipper(),
          child: Container(
            width: size,
            height: size,
            padding: const EdgeInsets.all(1),
            color: Palette.line2,
            child: ClipPath(
              clipper: const PortraitClipper(),
              child: Image.memory(
                data,
                fit: BoxFit.cover,
                gaplessPlayback: true,
                cacheWidth: (size * ratio).round(),
                errorBuilder: (context, error, stack) =>
                    const SizedBox.shrink(),
              ),
            ),
          ),
        );

        if (!zoom) {
          return Padding(padding: margin, child: portrait);
        }

        return Padding(
          padding: margin,
          child: Semantics(
            button: true,
            label: 'Show picture',
            child: GestureDetector(
              behavior: HitTestBehavior.opaque,
              onTap: () => push(_AvatarView(peer: peer, data: data)),
              child: portrait,
            ),
          ),
        );
      },
    );
  }
}

class _AvatarView extends StatelessWidget {
  const _AvatarView({required this.peer, required this.data});

  final String peer;
  final Uint8List data;

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: () => Navigator.of(context).maybePop(),
      child: ColoredBox(
        color: Palette.bg,
        child: SafeArea(
          child: Stack(
            children: [
              Center(
                child: LayoutBuilder(
                  builder: (context, box) {
                    final side =
                        math.min(box.maxWidth, box.maxHeight) -
                        Metrics.edge * 4;

                    return Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        ClipPath(
                          clipper: const PortraitClipper(),
                          child: Container(
                            width: side,
                            height: side,
                            padding: const EdgeInsets.all(2),
                            color: Palette.line2,
                            child: ClipPath(
                              clipper: const PortraitClipper(),
                              child: Image.memory(
                                data,
                                fit: BoxFit.cover,
                                filterQuality: FilterQuality.medium,
                              ),
                            ),
                          ),
                        ),
                        const SizedBox(height: 20),
                        PeerName(peer, style: Styles.title),
                      ],
                    );
                  },
                ),
              ),
              Positioned(
                top: 4,
                left: Metrics.edge - 12,
                child: GlyphBtn(
                  GlyphKind.close,
                  label: 'Close',
                  onTap: () => Navigator.of(context).maybePop(),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

Future<bool> confirm({
  required String title,
  required String text,
  required String action,
  Widget? detail,
  bool risky = false,
}) async {
  final context = navigatorKey.currentContext;
  if (context == null) {
    return false;
  }

  final result = await showModalBottomSheet<bool>(
    context: context,
    isScrollControlled: true,
    useSafeArea: true,
    backgroundColor: const Color(0x00000000),
    barrierColor: const Color(0xcc0b0c0e),
    elevation: 0,
    builder: (context) => CustomPaint(
      painter: const CutPainter(fill: Palette.bg2, stroke: Palette.line2),
      child: Padding(
        padding: EdgeInsets.fromLTRB(
          Metrics.edge,
          14,
          Metrics.edge,
          MediaQuery.paddingOf(context).bottom + 18,
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Center(
              child: Container(width: 32, height: 3, color: Palette.amber),
            ),
            const SizedBox(height: 20),
            Flexible(
              child: SingleChildScrollView(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        const RubricCross(fill: Palette.bg2),
                        const SizedBox(width: 12),
                        Expanded(child: Text(title, style: Styles.cardTitle)),
                      ],
                    ),
                    const SizedBox(height: 12),
                    Text(text, style: Styles.body),
                    if (detail != null) ...[const SizedBox(height: 16), detail],
                  ],
                ),
              ),
            ),
            const SizedBox(height: 24),
            BtnPair(
              first: Btn(
                label: 'Cancel',
                kind: risky ? BtnKind.primary : BtnKind.secondary,
                expand: true,
                onTap: () => Navigator.of(context).pop(false),
              ),
              second: Btn(
                label: action,
                kind: risky ? BtnKind.secondary : BtnKind.primary,
                expand: true,
                onTap: () => Navigator.of(context).pop(true),
              ),
            ),
          ],
        ),
      ),
    ),
  );

  return result ?? false;
}

Liveness liveness(Status status, String tunnelId) {
  if (status.connecting == tunnelId) {
    return Liveness.pending;
  }

  if (!status.running || status.tunnelId != tunnelId) {
    return Liveness.off;
  }

  if (!status.online) {
    return Liveness.pending;
  }

  return Liveness.online;
}

Future<void> toggle(Tunnel tunnel) async {
  final status = Vigil.instance.status;

  try {
    if (status.running && status.tunnelId == tunnel.id) {
      await Vigil.instance.disconnect();
    } else {
      await Vigil.instance.connect(tunnel.id);
    }
  } on VigilError catch (e) {
    notify(e.message);
  }
}
