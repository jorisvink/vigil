// The bridge to the native side. Kotlin and Swift own the stored tunnels
// and their keys, we only ever get to see the summaries from the core.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';

class Summary {
  Summary(this.json);

  final Map<String, dynamic> json;

  String get instance => json['instance'] as String? ?? '';
  String get mode => json['mode'] as String? ?? 'tunnel';
  String get address => json['address'] as String? ?? '';
  int get mtu => json['mtu'] as int? ?? 0;
  bool get mtuDiscovery => json['mtu_discovery'] as bool? ?? false;
  List<String> get routes => _strings('routes');
  List<String> get dns => _strings('dns');
  String? get peer => json['peer'] as String?;
  String? get cathedral => json['cathedral'] as String?;
  String? get spi => json['spi'] as String?;
  String? get flock => json['cathedral_flock'] as String?;
  String? get cathedralId => json['cathedral_id'] as String?;
  int? get natPort => json['cathedral_nat_port'] as int?;
  String? get liturgyPrefix => json['liturgy_prefix'] as String?;
  String? get liturgyGroup => json['liturgy_group'] as String?;
  bool get shroud => json['shroud'] as bool? ?? false;
  bool get p2p => json['p2p'] as bool? ?? false;
  bool get commixtion => json['commixtion'] as bool? ?? false;
  bool get blockIpv6 => json['block_ipv6'] as bool? ?? false;
  bool get discoverable => json['liturgy_discoverable'] as bool? ?? true;
  bool get tier6 => json['tier6'] as bool? ?? false;

  String get endpoint => peer ?? cathedral ?? '';

  List<String> _strings(String key) =>
      (json[key] as List<dynamic>? ?? const []).cast<String>();

  bool get voice => mode == 'gospel';

  String get modeName => switch (mode) {
    'cathedral' => 'Cathedral tunnel',
    'liturgy' => tier6 ? 'Tier6' : 'Liturgy',
    'gospel' => 'Gospel',
    _ => 'Direct tunnel',
  };

  List<(String, String)> get rows {
    final rows = <(String, String)>[('Mode', modeName)];

    if (peer != null) {
      rows.add(('Peer', peer!));
    }

    if (cathedral != null) {
      rows.add(('Cathedral', cathedral!));
      if (natPort != null) {
        rows.add(('NAT port', '$natPort'));
      }

      if (cathedralId != null) {
        rows.add(('Cathedral id', cathedralId!));
      }

      if (flock != null) {
        rows.add(('Flock', flock!));
      }

      if (spi != null) {
        final id = mode == 'liturgy' || voice;
        rows.add((
          id ? 'Peer id' : 'Tunnel spi',
          id ? spi!.substring(2) : spi!,
        ));
      }
    }

    if (liturgyGroup != null) {
      rows.add(('Liturgy group', liturgyGroup!));
    }

    if (liturgyPrefix != null) {
      rows.add(('Liturgy prefix', liturgyPrefix!));
    }

    if (!voice) {
      rows.add(('Address', address));
      rows.add(('Routes', routes.isEmpty ? '-' : routes.join('\n')));
      rows.add(('DNS', dns.isEmpty ? '-' : dns.join('\n')));
    }

    rows.add(('MTU', mtuDiscovery ? 'auto (up to $mtu)' : '$mtu'));
    rows.add(('Shroud', shroud ? 'yes' : 'no'));

    if (cathedral != null) {
      rows.add(('P2P', p2p ? 'yes' : 'no'));
      rows.add(('Commixtion', commixtion ? 'yes' : 'no'));
    }

    if (mode == 'liturgy') {
      rows.add(('Discoverable', discoverable ? 'yes' : 'no'));
    }

    if (!voice) {
      rows.add(('Block IPv6', blockIpv6 ? 'yes' : 'no'));
    }

    return rows;
  }
}

class Tunnel {
  Tunnel(this.id, this.summary);

  final String id;
  final Summary summary;

  factory Tunnel.fromJson(Map<String, dynamic> json) => Tunnel(
    json['id'] as String,
    Summary(json['summary'] as Map<String, dynamic>),
  );
}

class PeerStatus {
  PeerStatus(this.json);

  final Map<String, dynamic> json;

  String get id => json['id'] as String? ?? '00';
  String get address => json['address'] as String? ?? '';
  String? get tunnel => json['tunnel'] as String?;
  String? get cathedral => json['cathedral'] as String?;
  bool get online => json['online'] as bool? ?? false;
  bool get p2p => json['p2p'] as bool? ?? false;
  bool get cathedralAlive => json['cathedral_alive'] as bool? ?? false;
  String get txSpi => json['tx_spi'] as String? ?? '00000000';
  String get rxSpi => json['rx_spi'] as String? ?? '00000000';
  String get ambry => json['ambry'] as String? ?? '00000000';
  int get rxBytes => json['rx_bytes'] as int? ?? 0;
  int get txBytes => json['tx_bytes'] as int? ?? 0;
  int get rxPkts => json['rx_pkts'] as int? ?? 0;
  int get txPkts => json['tx_pkts'] as int? ?? 0;
  int get idle => json['idle'] as int? ?? 0;
  int get mtu => json['mtu'] as int? ?? 0;
}

class LogLine {
  LogLine(this.seq, this.when, this.line);

  final int seq;
  final String when;
  final String line;
}

class PhoneCall {
  PhoneCall(this.json);

  final Map<String, dynamic> json;

  String get peer => json['peer'] as String? ?? '00';
  bool get incoming => json['incoming'] as bool? ?? false;
  String get state => json['state'] as String? ?? 'dialing';
  bool get muted => json['muted'] as bool? ?? false;
  bool get speaker => json['speaker'] as bool? ?? false;
  bool get video => json['video'] as bool? ?? false;
  bool get front => json['front'] as bool? ?? true;
  int get since => json['since'] as int? ?? 0;
}

bool get videoCalls => Platform.isAndroid || Platform.isIOS;

class VideoState {
  VideoState(this.json);

  final Map<String, dynamic> json;

  bool get decoding => json['decoding'] as bool? ?? false;
  bool get capturing => json['capturing'] as bool? ?? false;
  bool get front => json['front'] as bool? ?? true;
  int get rotation => json['rotation'] as int? ?? 0;
  int get width => json['width'] as int? ?? 0;
  int get height => json['height'] as int? ?? 0;
  int get localRotation => json['localRotation'] as int? ?? 0;
  bool get localMirror => json['localMirror'] as bool? ?? false;
  int get localWidth => json['localWidth'] as int? ?? 960;
  int get localHeight => json['localHeight'] as int? ?? 540;
}

class ScanState {
  ScanState(this.json);

  final Map<String, dynamic> json;

  String? get value => json['value'] as String?;
  int get width => json['width'] as int? ?? 0;
  int get height => json['height'] as int? ?? 0;
}

class VoiceStatus {
  VoiceStatus(this.json);

  final Map<String, dynamic> json;

  bool get running => json['running'] as bool? ?? false;
  String get state => json['state'] as String? ?? 'connecting';
  bool get p2p => json['p2p'] as bool? ?? false;
  String? get path => json['path'] as String?;
  String? get cathedral => json['cathedral'] as String?;
  int get rxBytes => json['rx_bytes'] as int? ?? 0;
  int get txBytes => json['tx_bytes'] as int? ?? 0;

  Map<String, dynamic> get _video =>
      json['video'] as Map<String, dynamic>? ?? const {};

  bool get camera => _video['camera'] as bool? ?? false;
  bool get remoteCamera => _video['remote'] as bool? ?? false;
  int get videoBitrate => _video['bitrate'] as int? ?? 0;
}

class LiturgyStatus {
  LiturgyStatus(this.json);

  final Map<String, dynamic> json;

  String get kind => json['kind'] as String? ?? '';
  String get group => json['group'] as String? ?? '0000';
  bool get alive => json['alive'] as bool? ?? false;
  String? get cathedral => json['cathedral'] as String?;
  int get rx => json['rx'] as int? ?? 0;
  int get tx => json['tx'] as int? ?? 0;
}

class CallStatus {
  CallStatus(this.json);

  final Map<String, dynamic> json;

  bool get alive => json['alive'] as bool? ?? false;
  String? get cathedral => json['cathedral'] as String?;
  List<String> get peers =>
      (json['peers'] as List<dynamic>? ?? const []).cast<String>();
}

class ChatPeer {
  ChatPeer(this.json);

  final Map<String, dynamic> json;

  String get id => json['id'] as String? ?? '00';
  bool get present => json['present'] as bool? ?? false;
  bool get online => json['online'] as bool? ?? false;
  String get link => json['link'] as String? ?? 'none';
  bool get typing => json['typing'] as bool? ?? false;
  bool get p2p => json['p2p'] as bool? ?? false;
  int get queued => json['queued'] as int? ?? 0;
  String get nick => json['nick'] as String? ?? '';
  String get signed => json['signed'] as String? ?? '';

  String get typingTopic => json['typing_topic'] as String? ?? '';

  String get typingGroup {
    final group = json['typing_group'] as String?;

    if (group == null) {
      return '';
    }

    return 'g${json['typing_owner']}$group';
  }
}

class ChatStatus {
  ChatStatus(this.json);

  final Map<String, dynamic> json;

  bool get alive => json['alive'] as bool? ?? false;
  String get id => json['id'] as String? ?? '00';
  String get nick => json['nick'] as String? ?? '';
  String get signed => json['signed'] as String? ?? '';

  List<ChatPeer> get peers => (json['peers'] as List<dynamic>? ?? const [])
      .map((p) => ChatPeer(p as Map<String, dynamic>))
      .toList();

  ChatPeer? peer(String id) => peers.where((p) => p.id == id).firstOrNull;
}

class ChatGroup {
  ChatGroup(this.json);

  final Map<String, dynamic> json;

  String get owner => json['owner'] as String? ?? '00';
  String get id => json['id'] as String? ?? '';
  String get chat => 'g$owner$id';
  String get name => json['name'] as String? ?? '';
  String get state => json['state'] as String? ?? 'open';
  bool get open => state == 'open';

  List<String> get members =>
      (json['members'] as List<dynamic>? ?? const []).cast<String>();
}

bool isGroup(String chat) => chat.length == 19 && chat.startsWith('g');

class Chats {
  Chats(this.json);

  final Map<String, dynamic> json;

  static final Chats none = Chats(const {});

  int get version => json['version'] as int? ?? 0;
  int get avatars => json['avatars'] as int? ?? 0;
  int get names => json['names'] as int? ?? 0;
  int get unseen => json['unseen'] as int? ?? 0;
  bool get hardware => json['hardware'] as bool? ?? true;
  String? get open => json['open'] as String?;
  String? get openTopic => json['openTopic'] as String?;
}

class ChatMessage {
  ChatMessage(this.json);

  final Map<String, dynamic> json;

  int get ref => json['ref'] as int? ?? 0;
  bool get incoming => json['dir'] == 'in';
  String get text => json['text'] as String? ?? '';
  String get nick => json['nick'] as String? ?? '';
  String get from => json['peer'] as String? ?? '';
  String get kind => json['kind'] as String? ?? '';
  String get data => json['data'] as String? ?? '';
  String get media => json['media'] as String? ?? '';
  String get name => json['name'] as String? ?? '';
  String get state => json['state'] as String? ?? 'queued';
  int? get acked => json['acked'] as int?;
  int get pending => json['pending'] as int? ?? 0;
  bool get favorite => json['fav'] as bool? ?? false;
  bool get pinned => json['pinned'] as bool? ?? false;
  bool get gone => json['gone'] as bool? ?? false;
  bool get quoted => json['quoted'] as bool? ?? false;
  int get reply => json['reply'] as int? ?? 0;
  String get topic => json['tp'] as String? ?? '';
  DateTime get time =>
      DateTime.fromMillisecondsSinceEpoch(json['time'] as int? ?? 0);
}

class ChatSummary {
  ChatSummary(this.json);

  final Map<String, dynamic> json;

  String get peer => json['peer'] as String? ?? '00';
  int get unread => json['unread'] as int? ?? 0;
  bool get star => json['star'] as bool? ?? false;
  bool get compact => json['compact'] as bool? ?? false;
  int get order => json['order'] as int? ?? -1;
  ChatMessage? get last => json['last'] is Map<String, dynamic>
      ? ChatMessage(json['last'] as Map<String, dynamic>)
      : null;

  late final List<ChatTopic> topics =
      (json['topics'] as List<dynamic>? ?? const [])
          .map((t) => ChatTopic(t as Map<String, dynamic>))
          .toList();
}

class ChatTopic {
  ChatTopic(this.json);

  final Map<String, dynamic> json;

  String get id => json['id'] as String? ?? '';
  String get name => json['name'] as String? ?? '';
  int get color => json['color'] as int? ?? 0;
  int get unread => json['unread'] as int? ?? 0;
  ChatMessage? get last => json['last'] is Map<String, dynamic>
      ? ChatMessage(json['last'] as Map<String, dynamic>)
      : null;
}

class ChatHistory {
  ChatHistory(this.json);

  final Map<String, dynamic> json;

  String? get warning => json['warning'] as String?;
  int get unread => json['unread'] as int? ?? 0;
  int get epoch => json['epoch'] as int? ?? 0;
  int get stamp => json['stamp'] as int? ?? 0;
  bool get delta => json['delta'] as bool? ?? false;

  late final List<ChatMessage> messages =
      (json['messages'] as List<dynamic>? ?? const [])
          .map((m) => ChatMessage(m as Map<String, dynamic>))
          .toList();

  late final List<ChatTopic> topics =
      (json['topics'] as List<dynamic>? ?? const [])
          .map((t) => ChatTopic(t as Map<String, dynamic>))
          .toList();
}

class ChatSettings {
  ChatSettings(this.json);

  final Map<String, dynamic> json;

  String get nick => json['nick'] as String? ?? '';
  bool get preview => json['preview'] as bool? ?? false;
}

class Status {
  Status(
    this.json,
    this.tunnelId, [
    this.connecting,
    this.lockdown = false,
    this.phone,
    this.voice,
    Chats? chats,
    this.gospel = true,
    this.background = true,
    this.linked = true,
    this.linking = false,
  ]) : chats = chats ?? Chats.none;

  final Map<String, dynamic> json;

  final String? tunnelId;

  final String? connecting;
  final bool lockdown;
  final PhoneCall? phone;
  final VoiceStatus? voice;
  final Chats chats;
  final bool gospel;
  final bool background;
  final bool linked;
  final bool linking;

  static final Status idle = Status(const {'running': false}, null);

  bool get running => json['running'] as bool? ?? false;
  String get instance => json['instance'] as String? ?? '';
  String get mode => json['mode'] as String? ?? '';
  bool get tier6 => json['tier6'] as bool? ?? false;
  String get kind => tier6 ? 'tier6' : mode;
  int get uptime => json['uptime'] as int? ?? 0;
  String get address => json['address'] as String? ?? '';
  String? get cathedral => json['cathedral'] as String?;
  bool? get liturgyAlive => json['liturgy_alive'] as bool?;
  String? get liturgyCathedral => json['liturgy_cathedral'] as String?;

  List<String> get remembrances =>
      (json['remembrances'] as List<dynamic>? ?? const []).cast<String>();

  List<PeerStatus> get peers => (json['peers'] as List<dynamic>? ?? const [])
      .map((p) => PeerStatus(p as Map<String, dynamic>))
      .toList();

  List<LogLine> get log => (json['log'] as List<dynamic>? ?? const [])
      .map((l) => l as Map<String, dynamic>)
      .map(
        (l) => LogLine(
          l['seq'] as int? ?? 0,
          l['when'] as String? ?? '',
          l['line'] as String? ?? '',
        ),
      )
      .toList();

  bool get online => peers.any((p) => p.online);

  CallStatus? get call => json['call'] is Map<String, dynamic>
      ? CallStatus(json['call'] as Map<String, dynamic>)
      : null;

  List<LiturgyStatus> get liturgies => [
    for (final owner in [json['call'], json['chat']])
      if (owner is Map<String, dynamic>)
        for (final l in owner['liturgies'] as List<dynamic>? ?? const [])
          LiturgyStatus(l as Map<String, dynamic>),
  ];

  ChatStatus? get chat => json['chat'] is Map<String, dynamic>
      ? ChatStatus(json['chat'] as Map<String, dynamic>)
      : null;

  Map<String, dynamic> get _gospel =>
      json['gospel'] as Map<String, dynamic>? ?? const {};

  int get gospelRx =>
      (_gospel['rx_bytes'] as int? ?? 0) + (voice?.rxBytes ?? 0);
  int get gospelTx =>
      (_gospel['tx_bytes'] as int? ?? 0) + (voice?.txBytes ?? 0);
}

class VigilError implements Exception {
  VigilError(this.message, [this.code = '']);

  final String message;
  final String code;

  @override
  String toString() => message;
}

class Vigil {
  Vigil._();

  static final Vigil instance = Vigil._();

  static const _channel = MethodChannel('se.sanctorum.vigil/core');

  final _status = StreamController<Status>.broadcast();
  final _tunnels = StreamController<List<Tunnel>>.broadcast();

  Timer? _poll;
  Status _last = Status.idle;
  List<Tunnel> _list = const [];

  String? _connecting;
  DateTime _connectingSince = DateTime.now();

  Map<String, String> _names = const {};
  Map<String, ChatGroup> _groups = const {};
  final Map<String, Future<Uint8List?>> _data = {};
  static const _dataMax = 24;
  final Map<String, String> _nicks = {};
  final Map<String, String> _signed = {};
  final Map<String, Future<Uint8List?>> _avatars = {};
  int _avatarsSeen = 0;
  int? _namesSeen;
  bool _namesLoading = false;

  static const _connectLimit = Duration(seconds: 20);

  Status get status => _last;

  List<Tunnel> get tunnels => _list;

  Stream<Status> get statuses => _status.stream;

  Stream<List<Tunnel>> get tunnelUpdates => _tunnels.stream;

  Map<String, String> get names => _names;

  String name(String peer) {
    final named = _names[peer];
    if (named != null) {
      return named;
    }

    final signed = _signed[peer];
    if (signed != null) {
      return signed;
    }

    final nick = _nicks[peer];
    if (nick != null) {
      return chosen(nick);
    }

    if (isGroup(peer)) {
      final group = _groups[peer]?.name ?? '';

      return group.isEmpty ? 'Group' : group;
    }

    return 'Peer $peer';
  }

  Map<String, ChatGroup> get groups => _groups;

  ChatGroup? group(String chat) => _groups[chat];

  String mentionName(String peer) => _signed[peer] ?? _nicks[peer] ?? peer;

  Set<String> get mentionable {
    final chat = _last.chat;

    return {
      ..._signed.values,
      ..._nicks.values,
      if (chat != null) ...[
        chat.id,
        if (chat.nick.isNotEmpty) chat.nick,
        if (chat.signed.isNotEmpty) chat.signed,
      ],
      for (final g in _groups.values) ...g.members,
    };
  }

  String get selfName {
    final chat = _last.chat;

    if (chat == null) {
      return 'You';
    }

    if (chat.signed.isNotEmpty) {
      return chat.signed;
    }

    if (chat.nick.isNotEmpty) {
      return chosen(chat.nick);
    }

    return 'Peer ${chat.id}';
  }

  bool get selfPicked {
    final chat = _last.chat;

    return chat != null && chat.signed.isEmpty && chat.nick.isNotEmpty;
  }

  bool get selfSigned => _last.chat?.signed.isNotEmpty ?? false;

  Set<String> get selfNames {
    final chat = _last.chat;

    return {
      if (chat != null) ...[
        chat.id,
        if (chat.nick.isNotEmpty) chat.nick,
        if (chat.signed.isNotEmpty) chat.signed,
      ],
    };
  }

  bool picked(String peer) => !known(peer) && _nicks.containsKey(peer);

  bool known(String peer) =>
      _names.containsKey(peer) || _signed.containsKey(peer);

  bool authenticated(String peer) =>
      !_names.containsKey(peer) && _signed.containsKey(peer);

  void start() {
    _poll ??= Timer.periodic(const Duration(seconds: 1), (_) => refresh());
    refresh();
    reload();
    _loadNames(null).catchError((Object _) {});
  }

  Future<void> rename(String peer, String name) async {
    final next = Map.of(_names);

    if (name.trim().isEmpty) {
      next.remove(peer);
    } else {
      next[peer] = name.trim();
    }

    await _loadNames(jsonEncode(next));
  }

  Future<void> _loadNames(String? names) async {
    final raw = await _invoke('names', names);
    final all = jsonDecode(raw.isEmpty ? '{}' : raw) as Map<String, dynamic>;

    _names = {
      for (final entry in all.entries)
        if (RegExp(r'^[0-9a-f]{2}$').hasMatch(entry.key))
          entry.key: entry.value as String,
    };
    _publish(_last);
  }

  Future<void> refresh() async {
    try {
      final raw = await _channel.invokeMethod<String>('status');
      if (raw == null) {
        return;
      }

      final json = jsonDecode(raw) as Map<String, dynamic>;
      final status = Status(
        json['status'] as Map<String, dynamic>? ?? const {'running': false},
        json['tunnel'] as String?,
        null,
        json['lockdown'] as bool? ?? false,
        json['phone'] is Map<String, dynamic>
            ? PhoneCall(json['phone'] as Map<String, dynamic>)
            : null,
        json['voice'] is Map<String, dynamic>
            ? VoiceStatus(json['voice'] as Map<String, dynamic>)
            : null,
        json['chats'] is Map<String, dynamic>
            ? Chats(json['chats'] as Map<String, dynamic>)
            : null,
        json['gospel'] as bool? ?? true,
        json['background'] as bool? ?? true,
        json['link'] as bool? ?? true,
        json['linking'] as bool? ?? false,
      );

      final since = DateTime.now().difference(_connectingSince);

      if ((status.running &&
              status.tunnelId == _connecting &&
              status.uptime <= since.inSeconds + 1) ||
          since > _connectLimit) {
        _connecting = null;
      }

      _publish(status);
    } on PlatformException {
      return;
    } on FormatException {
      return;
    }
  }

  Future<List<Tunnel>> reload() async {
    final raw = await _channel.invokeMethod<String>('list') ?? '[]';
    _list = (jsonDecode(raw) as List<dynamic>)
        .map((t) => Tunnel.fromJson(t as Map<String, dynamic>))
        .toList();
    _tunnels.add(_list);
    return _list;
  }

  Future<Summary> check(String config) async {
    final bytes = utf8.encode(config);
    final String? raw;

    try {
      raw = await _channel.invokeMethod<String>('check', bytes);
    } finally {
      bytes.fillRange(0, bytes.length, 0);
    }

    final json = jsonDecode(raw ?? '{}') as Map<String, dynamic>;

    if (json.containsKey('error')) {
      throw VigilError(json['error'] as String);
    }

    return Summary(json);
  }

  Future<String?> handoff() async {
    final bytes = await _channel.invokeMethod<Uint8List>('handoff');

    if (bytes == null || bytes.isEmpty) {
      return null;
    }

    return utf8.decode(bytes, allowMalformed: true);
  }

  Future<Tunnel> store(String config) async {
    final bytes = utf8.encode(config);
    final String raw;

    try {
      raw = await _invoke('import', bytes);
    } finally {
      bytes.fillRange(0, bytes.length, 0);
    }

    final tunnel = Tunnel.fromJson(jsonDecode(raw) as Map<String, dynamic>);
    await reload();
    if (tunnel.summary.voice) {
      await _restart();
    }

    return tunnel;
  }

  /// Routes, p2p, tier6 and commixtion are null for tunnels that do not
  /// have them, the core decides if the result is valid.
  Future<Tunnel> edit(
    String id, {
    required String instance,
    List<String>? routes,
    required List<String> dns,
    bool? p2p,
    bool? tier6,
    bool? commixtion,
  }) async {
    final edits = [
      for (final r in routes ?? const <String>[]) 'route $r',
      for (final d in dns) 'dns $d',
      if (p2p != null) 'p2p ${p2p ? 'yes' : 'no'}',
      if (tier6 != null) 'tier6 ${tier6 ? 'yes' : 'no'}',
      if (commixtion != null)
        'cathedral_commixtion ${commixtion ? 'yes' : 'no'}',
      'instance $instance',
    ];

    final raw = await _invoke('edit', {
      'id': id,
      'edits': edits.map((e) => '$e\n').join(),
    });
    final tunnel = Tunnel.fromJson(jsonDecode(raw) as Map<String, dynamic>);
    await reload();
    return tunnel;
  }

  Future<void> remove(String id) async {
    final voice = _list.any((t) => t.id == id && t.summary.voice);

    await _invoke('remove', id);
    await reload();

    if (voice) {
      await _loadNames('{}');
      await _restart();
    }
  }

  Future<void> _restart() async {
    final id = _last.running ? _last.tunnelId : null;
    if (id != null) {
      await connect(id);
    }
  }

  Future<void> connect(String id) async {
    _connecting = id;
    _connectingSince = DateTime.now();
    _publish(_last);

    try {
      await _invoke('connect', id);
    } on VigilError {
      _connecting = null;
      _publish(_last);
      rethrow;
    }

    await reload();
    await refresh();
  }

  Future<void> disconnect() async {
    _connecting = null;
    _publish(_last);

    await _invoke('disconnect', null);
    await refresh();
  }

  Future<void> call(String peer, {bool video = false}) async {
    await _invoke('call', {'peer': peer, 'video': video});
    await refresh();
  }

  Future<void> answer({bool video = false}) async {
    await _invoke('answer', video);
    await refresh();
  }

  Future<void> camera(bool on) async {
    await _invoke('camera', on);
    await refresh();
  }

  Future<void> flip() => _invoke('flip', null);

  Future<(int, int)> videoTextures() async {
    final json = jsonDecode(await _invoke('videoTextures', null));
    final map = json as Map<String, dynamic>;

    return (map['remote'] as int, map['local'] as int);
  }

  Future<VideoState> videoState() async => VideoState(
    jsonDecode(await _invoke('videoState', null)) as Map<String, dynamic>,
  );

  Future<void> videoRelease() => _invoke('videoRelease', null);

  Future<int> scanStart() async {
    final json = jsonDecode(await _invoke('scanStart', null));
    return (json as Map<String, dynamic>)['texture'] as int;
  }

  Future<ScanState> scanState() async => ScanState(
    jsonDecode(await _invoke('scanState', null)) as Map<String, dynamic>,
  );

  Future<void> scanStop() => _invoke('scanStop', null);

  Future<void> eraseAll() => _invoke('eraseAll', null);

  Future<String?> scanImage(String path) async {
    final value = await _invoke('scanImage', path);
    return value.isEmpty ? null : value;
  }

  Future<void> decline() async {
    await _invoke('decline', null);
    await refresh();
  }

  Future<void> hangup() async {
    await _invoke('hangup', null);
    await refresh();
  }

  Future<void> mute(bool on) async {
    await _invoke('mute', on);
    await refresh();
  }

  Future<void> speaker(bool on) async {
    await _invoke('speaker', on);
    await refresh();
  }

  Future<List<ChatSummary>> chats() async {
    final raw = await _invoke('chats', null);
    return (jsonDecode(raw) as List<dynamic>)
        .map((c) => ChatSummary(c as Map<String, dynamic>))
        .toList();
  }

  Future<ChatHistory> chat(String peer, {int epoch = 0, int stamp = 0}) async =>
      ChatHistory(
        jsonDecode(
              await _invoke('chat', {
                'peer': peer,
                'epoch': epoch,
                'stamp': stamp,
              }),
            )
            as Map<String, dynamic>,
      );

  Future<void> send(
    String peer,
    String text, {
    int reply = 0,
    String topic = '',
  }) async {
    await _invoke('chatSend', {
      'peer': peer,
      'text': text,
      'reply': reply,
      'topic': topic,
    });
    await refresh();
  }

  Future<void> sendImage(String peer, String path, {String topic = ''}) async {
    await _invoke('chatImageSend', {
      'peer': peer,
      'path': path,
      'topic': topic,
    });
    await refresh();
  }

  Future<bool> clipboardImage() async =>
      await _invoke('clipboardImage', null) == 'true';

  Future<Uint8List> clipboardImageData() async {
    try {
      final data = await _channel.invokeMethod<Uint8List>('clipboardImageData');

      if (data == null) {
        throw VigilError('There is no image to paste.');
      }

      return data;
    } on PlatformException catch (e) {
      throw VigilError(e.message ?? e.code);
    }
  }

  Future<void> sendImageData(
    String peer,
    Uint8List data, {
    String topic = '',
  }) async {
    await _invoke('chatImageData', {
      'peer': peer,
      'data': data,
      'topic': topic,
    });
    await refresh();
  }

  Future<void> sendFile(String peer, {String topic = ''}) async {
    await _invoke('chatFileSend', {'peer': peer, 'topic': topic});
    await refresh();
  }

  Future<void> recordVoice(String peer) => _invoke('voiceRecord', peer);

  Future<void> sendVoice(String peer, {String topic = ''}) async {
    await _invoke('voiceSend', {'peer': peer, 'topic': topic});
    await refresh();
  }

  Future<String> createTopic(String peer, String name, int color) async {
    final id = await _invoke('chatTopic', {
      'peer': peer,
      'name': name,
      'color': color,
    });
    await refresh();
    return id;
  }

  Future<void> removeTopic(String peer, String topic) async {
    await _invoke('chatTopic', {
      'peer': peer,
      'topic': topic,
      'name': '',
      'color': 0,
    });
    await refresh();
  }

  Future<void> cancelVoice() => _invoke('voiceCancel', null);

  Future<void> playVoice(
    String peer,
    String data, {
    int at = 0,
    double rate = 1,
  }) => _invoke('voicePlay', {
    'peer': peer,
    'data': data,
    'at': at,
    'rate': rate,
  });

  Future<void> seekVoice(int at) => _invoke('voiceSeek', at);

  Future<void> speedVoice(double rate) => _invoke('voiceSpeed', rate);

  Future<(int, int)?> voicePosition() async {
    final parts = (await _invoke('voicePosition', null)).split(' ');

    if (parts.length != 2) {
      return null;
    }

    final at = int.tryParse(parts[0]);
    final total = int.tryParse(parts[1]);

    if (at == null || total == null) {
      return null;
    }

    return (at, total);
  }

  Future<void> stopVoice() => _invoke('voiceStop', null);

  Future<void> openData(String peer, String data, String name) =>
      _invoke('chatDataOpen', {'peer': peer, 'data': data, 'name': name});

  Future<bool> saveData(
    String peer,
    String data,
    String name,
    String media,
  ) async {
    final saved = await _invoke('chatDataSave', {
      'peer': peer,
      'data': data,
      'name': name,
      'media': media,
    });

    return saved == 'saved';
  }

  Future<Uint8List?> chatData(String peer, String data) {
    final key = '$peer/$data';
    final cached = _data.remove(key);

    if (cached != null) {
      _data[key] = cached;
      return cached;
    }

    final loading = _channel
        .invokeMethod<Uint8List>('chatData', {'peer': peer, 'data': data})
        .catchError((Object _) => null);

    _data[key] = loading;

    while (_data.length > _dataMax) {
      _data.remove(_data.keys.first);
    }

    return loading;
  }

  Future<Uint8List?> avatar(String peer) {
    if (peer.length != 2) {
      return Future.value(null);
    }

    final cached = _avatars[peer];
    if (cached != null) {
      return cached;
    }

    final loading = _channel
        .invokeMethod<Uint8List>('chatAvatar', peer)
        .catchError((Object _) {
          _avatars.remove(peer);
          return null;
        });

    _avatars[peer] = loading;

    return loading;
  }

  Future<void> setAvatar(Uint8List? image) async {
    await _invoke('chatAvatarSet', image);
    _avatars.clear();
    await refresh();
  }

  Future<void> read(String peer, {String topic = ''}) =>
      _invoke('chatRead', {'peer': peer, 'topic': topic});

  Future<void> retract(String peer, int ref, String data) async {
    await _invoke('chatRetract', {'peer': peer, 'ref': ref});
    _data.remove('$peer/$data');
    await refresh();
  }

  Future<void> favorite(String peer, int ref, bool on) async {
    await _invoke('chatFavorite', {'peer': peer, 'ref': ref, 'on': on});
    await refresh();
  }

  Future<void> pin(String peer, int ref, bool on) async {
    await _invoke('chatPin', {'peer': peer, 'ref': ref, 'on': on});
    await refresh();
  }

  Future<void> openChat(String peer, bool on) =>
      _invoke('chatOpen', {'peer': peer, 'on': on});

  Future<void> typing(String peer, bool on, {String topic = ''}) =>
      _invoke('chatTyping', {'peer': peer, 'on': on, 'topic': topic});

  Future<void> starChat(String peer, bool on) async {
    await _invoke('chatStar', {'peer': peer, 'on': on});
    await refresh();
  }

  Future<void> compactTopics(String peer, bool on) async {
    await _invoke('chatCompact', {'peer': peer, 'on': on});
    await refresh();
  }

  Future<void> orderChats(List<String> chats) async {
    await _invoke('chatOrder', chats);
    await refresh();
  }

  Future<void> deleteChat(String peer) async {
    await _invoke('chatDelete', peer);
    await refresh();
  }

  Future<void> gospel(bool on) async {
    await _invoke('gospel', on);
    await refresh();
  }

  Future<String> createGroup(String name, List<String> members) async {
    final chat = await _invoke('groupCreate', {
      'name': name,
      'members': members,
    });

    await refresh();
    return chat;
  }

  Future<void> editGroup(
    String chat, {
    String? name,
    List<String>? members,
  }) async {
    await _invoke('groupEdit', {
      'peer': chat,
      'name': name,
      'members': members,
    });
    await refresh();
  }

  Future<void> leaveGroup(String chat) async {
    await _invoke('groupLeave', chat);
    await refresh();
  }

  Future<int> chatMute(String peer, [int? until]) async =>
      int.parse(await _invoke('chatMute', {'peer': peer, 'until': until}));

  Future<ChatSettings> chatSettings({String? nick, bool? preview}) async =>
      ChatSettings(
        jsonDecode(
              await _invoke('chatSettings', {'nick': nick, 'preview': preview}),
            )
            as Map<String, dynamic>,
      );

  Future<bool> flag(String key, [bool? value]) async =>
      await _invoke('flag', {'key': key, 'value': value}) == 'true';

  Future<void> allowBackground() => _invoke('allowBackground', null);

  Future<List<String>> remoteWordList() async =>
      (await _invoke('remoteWordList', null)).split(' ');

  Future<Map<String, dynamic>?> remoteWords(String words) async {
    final bytes = utf8.encode(words);
    final String raw;

    try {
      raw = await _invoke('remoteWords', bytes);
    } finally {
      bytes.fillRange(0, bytes.length, 0);
    }

    if (raw.isEmpty) {
      return null;
    }

    return jsonDecode(raw) as Map<String, dynamic>;
  }

  Future<void> remoteStart(String id, String secret) =>
      _invoke('remoteStart', {'id': id, 'secret': secret});

  Future<int> remoteRun() async {
    try {
      return await _channel.invokeMethod<int>('remoteRun') ?? -1;
    } on PlatformException catch (e) {
      throw VigilError(e.message ?? e.code, e.code);
    }
  }

  Future<String?> remoteConfig() async {
    final bytes = await _channel.invokeMethod<Uint8List>('remoteConfig');

    if (bytes == null || bytes.isEmpty) {
      return null;
    }

    return utf8.decode(bytes);
  }

  Future<void> remoteStop() => _invoke('remoteStop', null);

  Future<String> version() async =>
      await _channel.invokeMethod<String>('version') ?? 'unknown';

  Future<String> kyrkaVersion() async =>
      await _channel.invokeMethod<String>('kyrkaVersion') ?? 'unknown';

  void _publish(Status status) {
    _last = Status(
      status.json,
      status.tunnelId,
      _connecting,
      status.lockdown,
      status.phone,
      status.voice,
      status.chats,
      status.gospel,
      status.background,
      status.linked,
      status.linking,
    );

    if (_last.chats.avatars != _avatarsSeen) {
      _avatarsSeen = _last.chats.avatars;
      _avatars.clear();
    }

    if (_last.chats.names != _namesSeen && !_namesLoading) {
      unawaited(_namesLoad(_last.chats.names));
    }

    for (final p in _last.chat?.peers ?? const <ChatPeer>[]) {
      if (p.nick.isNotEmpty) {
        _nicks[p.id] = p.nick;
      }
      _signedSeen(p.id, p.signed);
    }

    _status.add(_last);
  }

  void _signedSeen(String peer, String signed) {
    if (signed.isNotEmpty) {
      _signed[peer] = signed;
    }
  }

  Future<void> _namesLoad(int seen) async {
    _namesLoading = true;

    try {
      final groups = jsonDecode(await _invoke('chatGroups', null));

      _groups = {
        for (final entry in (groups as Map<String, dynamic>).entries)
          entry.key: ChatGroup(entry.value as Map<String, dynamic>),
      };

      final raw = jsonDecode(await _invoke('chatNames', null));

      for (final entry in (raw as Map<String, dynamic>).entries) {
        final name = entry.value as Map<String, dynamic>;
        final nick = name['nick'] as String? ?? '';
        final signed = name['signed'] as String? ?? '';

        if (nick.isEmpty) {
          _nicks.remove(entry.key);
        } else {
          _nicks[entry.key] = nick;
        }

        if (signed.isEmpty) {
          _signed.remove(entry.key);
        } else {
          _signed[entry.key] = signed;
        }
      }

      _namesSeen = seen;
      _status.add(_last);
    } on VigilError catch (_) {
      return;
    } finally {
      _namesLoading = false;
    }
  }

  Future<String> _invoke(String method, Object? arg) async {
    try {
      return await _channel.invokeMethod<String>(method, arg) ?? '';
    } on PlatformException catch (e) {
      throw VigilError(e.message ?? e.code, e.code);
    }
  }
}

String chosen(String nick) => '~$nick';

const selfPeer = '00';

String bytes(int value) {
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  var v = value.toDouble();
  var unit = 0;

  while (v >= 1024 && unit < units.length - 1) {
    v /= 1024;
    unit++;
  }

  if (unit == 0) {
    return '$value B';
  }

  return '${v.toStringAsFixed(1)} ${units[unit]}';
}

String duration(int seconds) {
  final d = seconds ~/ 86400;
  final h = (seconds % 86400) ~/ 3600;
  final m = (seconds % 3600) ~/ 60;
  final s = seconds % 60;
  String two(int v) => v.toString().padLeft(2, '0');

  if (d > 0) {
    return '${d}d ${two(h)}:${two(m)}:${two(s)}';
  }

  return '${two(h)}:${two(m)}:${two(s)}';
}
