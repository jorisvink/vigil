import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'app.dart';
import 'desktop/shell.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();

  if (Platform.isLinux) {
    runApp(const DesktopApp());
    return;
  }

  SystemChrome.setSystemUIOverlayStyle(
    const SystemUiOverlayStyle(
      statusBarColor: Color(0x00000000),
      statusBarIconBrightness: Brightness.light,
      statusBarBrightness: Brightness.dark,
      systemNavigationBarColor: Color(0xff1e1a16),
      systemNavigationBarIconBrightness: Brightness.light,
    ),
  );

  runApp(const VigilApp());
}
