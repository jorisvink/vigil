import 'dart:async';
import 'dart:convert';
import 'dart:io';

const reliquary = 'https://vessel.reliquary.se/v1';

const wordCount = 33;

class ReliquaryError implements Exception {
  ReliquaryError(this.message);

  final String message;

  @override
  String toString() => message;
}

Future<Map<String, dynamic>> reliquaryCreate(
  String api,
  String flock,
  List<int> cosk,
) async {
  final base = api.trim().replaceAll(RegExp(r'/+$'), '');
  final uri = Uri.tryParse('$base/device/$flock/create');

  if (uri == null ||
      (uri.scheme != 'https' &&
          !(uri.scheme == 'http' && uri.host == '127.0.0.1'))) {
    throw ReliquaryError('Reliquary must be an https address.');
  }

  final client = HttpClient()..connectionTimeout = const Duration(seconds: 15);

  try {
    final request = await client.postUrl(uri);
    request.headers.contentType = ContentType.binary;
    request.contentLength = cosk.length;
    request.add(cosk);

    final response = await request.close().timeout(const Duration(seconds: 30));
    final body = await response.transform(utf8.decoder).join();

    if (response.statusCode != 200) {
      throw ReliquaryError(
        'Reliquary did not let us join (${response.statusCode}).',
      );
    }

    final json = jsonDecode(body) as Map<String, dynamic>;
    final id = json['cathedral_id'] as String? ?? '';
    final secret = json['cathedral_secret'] as String? ?? '';

    if (!RegExp(r'^[0-9a-f]{8}$').hasMatch(id) ||
        !RegExp(r'^[0-9a-f]{64}$').hasMatch(secret)) {
      throw ReliquaryError(
        'Reliquary sent back something we do not understand.',
      );
    }

    return json;
  } on SocketException catch (e) {
    throw ReliquaryError('Could not reach Reliquary: ${e.message}');
  } on TimeoutException {
    throw ReliquaryError('Reliquary did not answer in time.');
  } on FormatException {
    throw ReliquaryError('Reliquary sent back something we do not understand.');
  } finally {
    client.close(force: true);
  }
}
