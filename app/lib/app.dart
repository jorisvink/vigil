import 'dart:async';

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
      if (peer != null && s.phone == null) {
        goTab(NavTab.gospel);
        push(ChatPage(peer: peer));
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

Route<T> route<T>(Widget page) {
  return PageRouteBuilder<T>(
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

Future<T?> push<T>(Widget page) {
  return navigatorKey.currentState!.push<T>(route<T>(page));
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

Future<bool> confirm({
  required String title,
  required String text,
  required String action,
}) async {
  final context = navigatorKey.currentContext;
  if (context == null) {
    return false;
  }

  final result = await showModalBottomSheet<bool>(
    context: context,
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
            Row(
              children: [
                const RubricCross(fill: Palette.bg2),
                const SizedBox(width: 12),
                Expanded(child: Text(title, style: Styles.cardTitle)),
              ],
            ),
            const SizedBox(height: 12),
            Text(text, style: Styles.body),
            const SizedBox(height: 24),
            BtnPair(
              first: Btn(
                label: 'Cancel',
                kind: BtnKind.secondary,
                expand: true,
                onTap: () => Navigator.of(context).pop(false),
              ),
              second: Btn(
                label: action,
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
