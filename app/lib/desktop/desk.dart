import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import '../vigil.dart' show flockOf, localOf;

const companionRef = 1 << 62;

const topicColors = [
  0xffb000,
  0x6ee7a0,
  0x5ab4ff,
  0xff6b6b,
  0xc58cff,
  0xe8e6e1,
];

const topicNameMax = 32;
const groupNameMax = 32;
const messageMax = 512;

const _creating = {'msg', 'member', 'topic'};

class DeskError implements Exception {
  DeskError(this.code, this.message);

  final String code;
  final String message;

  bool get cancelled => code == 'cancelled';

  @override
  String toString() => message;
}

enum Delivery { queued, sent, delivered, failed }

class Message {
  Message({
    required this.chat,
    required this.ref,
    required this.out,
    required this.author,
    required this.at,
    this.text = '',
    this.kind = '',
    this.blob = '',
    this.name = '',
    this.size = 0,
    this.hash = '',
    this.quoteAuthor = '',
    this.quoteHash = '',
    this.topic = '',
    this.system = '',
  });

  final String chat;
  final int ref;
  final bool out;
  final String author;
  final int at;
  final String text;
  final String kind;
  final String blob;
  final String name;
  final int size;
  final String hash;
  final String quoteAuthor;
  final String quoteHash;
  final String topic;
  final String system;

  Delivery delivery = Delivery.queued;
  final Set<String> to = {};
  final Set<String> got = {};
  String? digest;
  bool gone = false;
  bool favorite = false;
  bool pinned = false;

  String get key => '$chat:$ref';

  String get file {
    if (gone) {
      return '';
    }

    return blob;
  }

  bool get data => kind.isNotEmpty;

  bool get image => kind == 'image';

  bool get voice => kind == 'voice';

  String get media => kind;

  String get content {
    if (kind.isNotEmpty) {
      return name;
    }

    return text;
  }

  String get summary {
    if (gone) {
      return 'Removed';
    }

    if (image) {
      return 'Image';
    }

    if (voice) {
      return 'Voice message';
    }

    if (kind.isNotEmpty) {
      return name;
    }

    return text;
  }

  void tally() {
    final acked = got.length;

    if (acked > 0 && to.length - acked <= 0) {
      delivery = Delivery.delivered;
    } else if (acked > 0) {
      delivery = Delivery.sent;
    } else if (to.isEmpty) {
      delivery = Delivery.failed;
    }
  }

  bool unaddress(String member) {
    if (got.contains(member) || !to.remove(member)) {
      return false;
    }

    tally();
    return true;
  }
}

class Topic {
  Topic(this.chat, this.id);

  final Chat chat;
  final String id;

  String name = '';
  int color = topicColors.first;

  int get unread => chat.unreads[id] ?? 0;
}

class Chat {
  Chat(this.id);

  final String id;

  final List<Message> messages = [];
  final Map<int, Message> index = {};
  final Map<String, Topic> topics = {};
  final Map<String, int> unreads = {};
  int unread = 0;
  int last = 0;
  int pinned = 0;

  Map<String, dynamic> get _info => Desk.instance._groups[id] ?? const {};

  bool get isGroup => RegExp(r'^[0-9a-f]{1,16}-g[0-9a-f]{18}$').hasMatch(id);

  String get flock => flockOf(id);

  String get _local => localOf(id);

  String get owner => isGroup ? '$flock-${_local.substring(1, 3)}' : '';

  String get group => isGroup ? _local.substring(3) : '';

  String get name => _info['name'] as String? ?? '';

  String get state => _info['state'] as String? ?? 'open';

  int get serial => _info['serial'] as int? ?? 0;

  Set<String> get members => {
    for (final m in _info['members'] as List<dynamic>? ?? const [])
      '$flock-$m',
  };

  bool get closed => isGroup && state != 'open';

  bool get starred => Desk.instance._stars.containsKey(id);

  Iterable<Topic> get named => topics.values.where((t) => t.name.isNotEmpty);

  int get muted => Desk.instance._mutes[id] as int? ?? 0;

  String get peer {
    if (isGroup) {
      return '';
    }

    return id;
  }

  int get unreadAll => unreads.entries
      .where((e) => topics.containsKey(e.key))
      .fold(unread, (sum, e) => sum + e.value);

  bool get silenced {
    if (muted == 0) {
      return false;
    }

    return muted < 0 || muted > DateTime.now().millisecondsSinceEpoch;
  }

  int unreadIn(String topic) {
    if (topic.isEmpty) {
      return unread;
    }

    return unreads[topic] ?? 0;
  }

  void _add(Message message) {
    final had = index[message.ref];

    if (had != null) {
      messages[messages.indexOf(had)] = message;
    } else {
      messages.add(message);
    }

    index[message.ref] = message;

    if (message.at > last) {
      last = message.at;
    }
  }

  void _remove(int ref) {
    final had = index.remove(ref);

    if (had != null) {
      messages.remove(had);
    }
  }
}

class Peer {
  Peer(this.id);

  final String id;

  bool present = false;
  bool online = false;
  bool typing = false;
  String owner = '';
  String group = '';
  String topic = '';
}

class Call {
  Call({
    required this.peer,
    required this.state,
    required this.relay,
    required this.ringing,
    required this.outgoing,
  });

  final String peer;
  final int state;
  final bool relay;
  final bool ringing;
  final bool outgoing;
}

class CallEnd {
  CallEnd({required this.peer, required this.text, required this.at});

  final String peer;
  final String text;
  final DateTime at;
}

class Desk {
  Desk._();

  static final Desk instance = Desk._();

  static const _channel = MethodChannel('se.sanctorum.vigil/desktop');
  static const _lagged = 3;
  static const _endShown = Duration(seconds: 4);

  final changes = ValueNotifier<int>(0);
  final problems = ValueNotifier<String>('');

  final Map<String, Chat> chats = {};
  final Map<String, Peer> peers = {};
  final Map<String, String> avatars = {};
  final Map<String, String> _nicks = {};
  final Map<String, String> _signed = {};
  final Map<String, Map<String, dynamic>> _groups = {};
  final Map<String, Future<Uint8List?>> _data = {};

  Map<String, dynamic> _stars = {};
  Map<String, dynamic> _mutes = {};
  Map<String, dynamic> _labels = {};
  List<String> _order = [];

  final Map<String, String> selves = {};
  final Map<String, String> _selfSigned = {};
  String selfAvatar = '';
  bool linked = false;
  bool online = false;
  String sync = 'synced';
  String outdated = '';
  int behind = 0;
  int _lagging = 0;
  String link = 'none';
  int queued = 0;
  String? open;
  String openTopic = '';
  bool focused = true;

  List<String> incoming = const [];
  Call? call;
  CallEnd? ended;
  int _endedCount = 0;
  final Set<String> _sending = {};
  Map<String, dynamic> voice = const {};

  Timer? _poll;
  Timer? _status;
  int _lastRef = 0;
  bool _opened = false;
  String _said = '';

  Future<T?> _invoke<T>(String method, [Object? args]) async {
    try {
      return await _channel.invokeMethod<T>(method, args);
    } on PlatformException catch (e) {
      throw DeskError(e.code, e.message ?? e.code);
    }
  }

  Future<void> unlock() async {
    if (!_opened) {
      linked = await _invoke<bool>('unlock') ?? false;
      _opened = true;
    }

    final raw = await _invoke<String>('records') ?? '[]';

    for (final record in jsonDecode(raw) as List<dynamic>) {
      _record(record as Map<String, dynamic>, replay: true);
    }

    _poll ??= Timer.periodic(
      const Duration(milliseconds: 250),
      (_) => _pull(),
    );
    _status ??= Timer.periodic(
      const Duration(milliseconds: 500),
      (_) => _refresh(),
    );

    _changed();
  }

  Future<void> importConfig() async {
    if (await _invoke<bool>('import') ?? false) {
      linked = true;
      _changed();
    }
  }

  Future<bool> remoteLink() async {
    if (!(await _invoke<bool>('remoteLink') ?? false)) {
      return false;
    }

    linked = true;
    _changed();
    return true;
  }

  Future<void> pasteConfig(String config) async {
    if (await _invoke<bool>('paste', config) ?? false) {
      linked = true;
      _changed();
    }
  }

  Future<void> resync() async {
    await _invoke<void>('resync');
    sync = 'unsynced';
    _changed();
  }

  Future<void> unlink() async {
    await _invoke<void>('unlink');
    linked = false;
    online = false;
    _changed();
  }

  Future<void> _pull() async {
    try {
      final raw = await _invoke<String>('poll') ?? '[]';
      final list = jsonDecode(raw) as List<dynamic>;

      if (list.isEmpty) {
        return;
      }

      for (final record in list) {
        _record(record as Map<String, dynamic>);
      }

      _changed();

      final chat = chats[open];

      if (chat != null) {
        await digests(chat);
      }
    } on DeskError {
      return;
    }
  }

  Future<void> _refresh() async {
    try {
      final raw = await _invoke<String>('status') ?? '{}';
      final json = jsonDecode(raw) as Map<String, dynamic>;
      final companion =
          (json['chat'] as Map<String, dynamic>?)?['companion']
              as Map<String, dynamic>? ??
          const {};

      online = companion['online'] as bool? ?? false;
      sync = companion['sync'] as String? ?? 'synced';
      outdated = _outdated(companion);
      final count = companion['behind'] as int? ?? 0;

      if (count == 0) {
        _lagging = 0;
      } else {
        _lagging++;
      }

      behind = 0;

      if (_lagging >= _lagged) {
        behind = count;
      }
      link = companion['link'] as String? ?? 'none';
      queued = companion['queued'] as int? ?? 0;

      _presence(companion);

      if (call != null || incoming.isNotEmpty || voice.isNotEmpty) {
        final state = await _invoke<String>('voice') ?? '{}';
        final parsed = jsonDecode(state) as Map<String, dynamic>;

        if (parsed['running'] == true) {
          voice = parsed;
        } else {
          voice = const {};
        }
      }

      final shown = ended;

      if (shown != null && DateTime.now().difference(shown.at) > _endShown) {
        ended = null;
      }

      final now = jsonEncode([
        online,
        sync,
        outdated,
        behind,
        ended?.text,
        link,
        queued,
        companion['peers'],
        companion['typed'],
        companion['incoming'],
        companion['call'],
        voice['state'],
        voice['muted'],
        voice['uptime'],
      ]);

      if (now != _said) {
        _said = now;
        _changed();
      }
    } on DeskError {
      return;
    } on FormatException {
      return;
    }
  }

  String _outdated(Map<String, dynamic> companion) {
    final version = companion['version'] as int? ?? 0;
    final other = companion['other'] as int?;

    if (!online || other == null) {
      return '';
    }

    if (other < version) {
      return 'phone';
    }

    if (other > version) {
      return 'desktop';
    }

    return '';
  }

  void _presence(Map<String, dynamic> companion) {
    peers.clear();

    for (final p in companion['peers'] as List<dynamic>? ?? const []) {
      final json = p as Map<String, dynamic>;
      final peer = Peer(json['id'] as String);

      peer.present = json['present'] as bool? ?? false;
      peer.online = json['online'] as bool? ?? false;
      peer.typing = json['typing'] as bool? ?? false;
      peers[peer.id] = peer;
    }

    for (final t in companion['typed'] as List<dynamic>? ?? const []) {
      final json = t as Map<String, dynamic>;
      final peer = peers[json['id'] as String];

      if (peer == null) {
        continue;
      }

      peer.owner = json['owner'] as String? ?? '';
      peer.group = json['group'] as String? ?? '';
      peer.topic = json['topic'] as String? ?? '';
    }

    incoming = (companion['incoming'] as List<dynamic>? ?? const [])
        .cast<String>();

    final json = companion['call'] as Map<String, dynamic>?;
    final end = companion['ended'] as Map<String, dynamic>? ?? const {};
    final count = end['count'] as int? ?? 0;
    final before = call;

    if (json == null && before != null) {
      var text = 'Call ended';

      if (count > _endedCount && end['peer'] == before.peer) {
        text = _endText(before, end['why'] as String? ?? '');
      }

      ended = CallEnd(peer: before.peer, text: text, at: DateTime.now());
    }

    _endedCount = count;

    if (json == null) {
      call = null;
    } else {
      ended = null;
      call = Call(
        peer: json['peer'] as String,
        state: json['state'] as int? ?? 0,
        relay: json['relay'] as bool? ?? false,
        ringing: json['ringing'] as bool? ?? false,
        outgoing: json['outgoing'] as bool? ?? false,
      );
    }
  }

  String _endText(Call call, String why) {
    final talking = call.state == 3;

    switch (why) {
      case 'remote':
        if (talking) {
          return '${name(call.peer)} hung up';
        }

        if (call.outgoing) {
          return '${name(call.peer)} did not pick up';
        }

        return 'Missed call';
      case 'timeout':
        if (talking) {
          return 'Lost the connection';
        }

        return 'No answer';
      case 'local':
        return 'Call ended';
      default:
        return 'Call failed';
    }
  }

  void _changed() => changes.value++;

  String selfIn(String chat) => selves[flockOf(chat)] ?? '';

  bool isSelf(String peer) => peer == '00' || selves.containsValue(peer);

  String selfSigned(String peer) => _selfSigned[flockOf(peer)] ?? '';

  String get selfLabel {
    final signed = _selfSigned.values.where((s) => s.isNotEmpty).toSet();

    if (signed.length != 1) {
      return 'You';
    }

    return signed.first;
  }

  String author(Message message) {
    if (mine(message)) {
      return name(selfIn(message.chat).isEmpty ? '00' : selfIn(message.chat));
    }

    return name(message.author);
  }

  String name(String peer) {
    if (isSelf(peer)) {
      if (selfSigned(peer).isNotEmpty) {
        return selfSigned(peer);
      }

      return 'You';
    }

    final label = _labels[peer] as String?;

    if (label != null) {
      return label;
    }

    final signed = _signed[peer];

    if (signed != null) {
      return signed;
    }

    final nick = _nicks[peer];

    if (nick != null) {
      return '~$nick';
    }

    return peer;
  }

  String mentionName(String peer) {
    if (isSelf(peer)) {
      if (selfSigned(peer).isNotEmpty) {
        return selfSigned(peer);
      }

      return localOf(peer);
    }

    return _signed[peer] ?? _nicks[peer] ?? peer;
  }

  Set<String> selfNames(String chat) => {
    if (selfIn(chat).isNotEmpty) localOf(selfIn(chat)),
    if (selfSigned(chat).isNotEmpty) selfSigned(chat),
  };

  Set<String> get mentionable => {
    ..._signed.values,
    ..._nicks.values,
    ..._selfSigned.values,
    for (final chat in chats.values) ...chat.members.map(localOf),
  };

  String title(Chat chat) {
    if (!chat.isGroup) {
      return name(chat.id);
    }

    if (chat.name.isEmpty) {
      return 'Group';
    }

    return chat.name;
  }

  bool mine(Message message) => message.out || isSelf(message.author);

  bool owns(Chat chat) {
    if (!chat.isGroup) {
      return true;
    }

    return chat.owner == selfIn(chat.id) && !chat.closed;
  }

  List<Chat> get sorted {
    final list = chats.values.toList();

    list.sort((a, b) {
      if (a.starred != b.starred) {
        return a.starred ? -1 : 1;
      }

      final x = _order.indexOf(a.id);
      final y = _order.indexOf(b.id);

      if (x != y) {
        if (x == -1) {
          return 1;
        }

        if (y == -1) {
          return -1;
        }

        return x.compareTo(y);
      }

      final order = title(a).toLowerCase().compareTo(title(b).toLowerCase());

      if (order != 0) {
        return order;
      }

      return a.id.compareTo(b.id);
    });

    return list;
  }

  int get unread => chats.values.fold(0, (sum, c) => sum + c.unreadAll);

  List<String> get known {
    final ids = <String>{
      ..._nicks.keys,
      ..._signed.keys,
      ..._labels.keys,
      for (final peer in peers.values)
        if (peer.present || peer.online) peer.id,
      for (final chat in chats.values)
        if (!chat.isGroup) chat.id,
      for (final chat in chats.values) ...chat.members,
    };

    ids.removeAll(selves.values);
    ids.remove('00');

    final list = ids.where(
      (id) => RegExp(r'^[0-9a-f]{1,16}-[0-9a-f]{2}$').hasMatch(id),
    );
    return list.toList()..sort((a, b) => name(a).compareTo(name(b)));
  }

  bool around(String peer) {
    final state = peers[peer];

    if (state == null) {
      return false;
    }

    return state.online || state.present;
  }

  List<String> typers(Chat chat, String topic) {
    final out = <String>[];

    for (final peer in peers.values) {
      if (chat.isGroup) {
        if (flockOf(peer.id) != chat.flock) {
          continue;
        }

        if ('${chat.flock}-${peer.owner}' != chat.owner ||
            peer.group != chat.group) {
          continue;
        }
      } else if (!peer.typing || peer.id != chat.id || peer.group.isNotEmpty) {
        continue;
      }

      if (peer.topic != topic) {
        continue;
      }

      out.add(peer.id);
    }

    return out;
  }

  String? get ringingFor {
    final now = call;

    if (now != null) {
      if (now.ringing) {
        return now.peer;
      }

      return null;
    }

    if (incoming.isEmpty) {
      return null;
    }

    return incoming.first;
  }

  Chat chat(String id) => chats.putIfAbsent(id, () => Chat(id));

  void _record(Map<String, dynamic> record, {bool replay = false}) {
    final file = record['file'] as String? ?? '';
    final ops = record['events'] as List<dynamic>;

    for (var i = 0; i < ops.length; i++) {
      final last = i == ops.length - 1;

      _op(ops[i] as Map<String, dynamic>, last ? file : '', replay);
    }
  }

  void _op(Map<String, dynamic> op, String file, bool replay) {
    final chat = op['chat'] as String? ?? '';

    switch (op['op']) {
      case 'self':
        selves.clear();
        _selfSigned.clear();
        for (final g in op['gospels'] as List<dynamic>? ?? const []) {
          final gospel = g as Map<String, dynamic>;
          final flock = gospel['flock'] as String;

          selves[flock] = '$flock-${gospel['id']}';
          _selfSigned[flock] = gospel['signed'] as String? ?? '';
        }
        if (op.containsKey('avatar') || file.isNotEmpty) {
          selfAvatar = file;
        }
      case 'rec':
        _rec(chat, op['rec'] as Map<String, dynamic>, replay);
      case 'batch':
        for (final inner in op['ops'] as List<dynamic>) {
          _op(inner as Map<String, dynamic>, '', replay);
        }
      case 'data':
        _data.remove(op['file']);
      case 'chat':
        _forget(chat);
      case 'delete':
        _forget(chat);
        _groups.remove(chat);
      case 'reset':
        for (final id in chats.keys.toList()) {
          _forget(id);
        }
        avatars.clear();
        _data.clear();
        _groups.clear();
        _nicks.clear();
        _signed.clear();
      case 'read':
        _read(chat, op['topic'] as String? ?? '');
      case 'group':
        final info = op['info'];
        if (info is Map<String, dynamic>) {
          _groups[chat] = info;
        } else {
          _groups.remove(chat);
        }
      case 'name':
        final peer = op['peer'] as String;
        _set(_nicks, peer, op['nick'] as String? ?? '');
        _set(_signed, peer, op['signed'] as String? ?? '');
      case 'avatar':
        final peer = op['peer'] as String;
        _data.remove(avatars[peer]);
        _set(avatars, peer, file);
      case 'avatars':
        final keep = (op['peers'] as List<dynamic>).cast<String>().toSet();
        avatars.removeWhere((peer, _) => !keep.contains(peer));
      case 'prefs':
        _stars = op['stars'] as Map<String, dynamic>? ?? {};
        _mutes = op['mutes'] as Map<String, dynamic>? ?? {};
        _labels = op['names'] as Map<String, dynamic>? ?? {};
        _order = (op['order'] as List<dynamic>? ?? const []).cast<String>();
      case 'answer':
        if (op['ok'] == false && !replay) {
          problems.value = op['reason'] as String? ?? 'Your phone said no.';
        }

        _unsent(chat, op['ref'] as int? ?? 0);
      case 'queued':
        _queued(op['event'] as Map<String, dynamic>);
    }
  }

  void _set(Map<String, String> map, String key, String value) {
    if (value.isEmpty) {
      map.remove(key);
    } else {
      map[key] = value;
    }
  }

  void _rec(String id, Map<String, dynamic> rec, bool replay) {
    final type = rec['t'] as String? ?? '';
    final ref = rec['ref'] as int? ?? 0;

    if (!chats.containsKey(id) && !_creating.contains(type)) {
      return;
    }

    final chat = this.chat(id);

    switch (type) {
      case 'msg':
        _incoming(chat, rec, replay);
      case 'member':
        final message = _message(chat, rec, system: rec['kind'] as String);
        chat._add(message);
        if (message.system == 'left') {
          for (final m in chat.messages) {
            m.unaddress(message.author);
          }
        }
      case 'sent':
        final message = chat.index[ref];
        if (message != null && message.delivery == Delivery.queued) {
          message.delivery = Delivery.sent;
        }
      case 'delivered':
        final message = chat.index[ref];
        final member = rec['member'] as String?;
        if (message != null && member == null) {
          message.delivery = Delivery.delivered;
        } else if (message != null) {
          message.got.add(member!);
          message.tally();
        }
      case 'expired':
        chat.index[ref]?.unaddress(rec['member'] as String? ?? '');
      case 'fav':
        chat.index[ref]?.favorite = rec['on'] as bool? ?? false;
      case 'pin':
        final before = chat.pinned;
        if (rec['on'] == true) {
          chat.pinned = ref;
        } else if (chat.pinned == ref) {
          chat.pinned = 0;
        }
        chat.index[before]?.pinned = false;
        chat.index[chat.pinned]?.pinned = true;
      case 'topic':
        _topic(chat, rec, replay);
      case 'retract':
        _retracted(chat, rec, replay);
    }
  }

  void _incoming(
    Chat chat,
    Map<String, dynamic> rec,
    bool replay,
  ) {
    final ref = rec['ref'] as int;

    _saw(ref);

    if (chat.index.containsKey(ref) && !_sending.contains('${chat.id}:$ref')) {
      return;
    }

    final message = _message(chat, rec);
    final topic = message.topic;

    if (_sending.remove('${chat.id}:$ref')) {
      chat._remove(ref);
    }

    chat._add(message);

    if (message.out) {
      return;
    }

    if (topic.isEmpty) {
      chat.unread++;
    } else {
      chat.unreads[topic] = (chat.unreads[topic] ?? 0) + 1;
    }

    if (!replay && chat.id == open && openTopic == topic && focused) {
      unawaited(read(chat, topic).catchError((_) {}));
    }
  }

  Message _message(Chat chat, Map<String, dynamic> rec, {String system = ''}) {
    final out = rec['dir'] == 'out';
    final message = Message(
      chat: chat.id,
      ref: rec['ref'] as int,
      out: out,
      author: out ? '00' : _from(chat, rec['peer'] as String? ?? ''),
      at: rec['time'] as int? ?? 0,
      text: rec['text'] as String? ?? '',
      kind: rec['media'] as String? ?? '',
      blob: rec['data'] as String? ?? '',
      name: rec['name'] as String? ?? '',
      size: rec['size'] as int? ?? 0,
      hash: rec['ch'] as String? ?? '',
      quoteAuthor: rec['qa'] as String? ?? '',
      quoteHash: rec['qh'] as String? ?? '',
      topic: rec['tp'] as String? ?? '',
      system: system,
    );

    if (!out) {
      message.delivery = Delivery.delivered;
    }

    final to = rec['to'] as List<dynamic>?;

    if (to != null) {
      message.to.addAll(to.cast<String>());
      message.tally();
    }

    return message;
  }

  String _from(Chat chat, String peer) {
    if (peer.isEmpty || peer == '00') {
      return peer;
    }

    return '${chat.flock}-$peer';
  }

  void _topic(Chat chat, Map<String, dynamic> rec, bool replay) {
    final id = rec['tp'] as String;
    final name = rec['name'] as String? ?? '';

    if (name.isNotEmpty) {
      final topic = chat.topics.putIfAbsent(id, () => Topic(chat, id));

      topic.name = name;
      topic.color = rec['color'] as int? ?? topicColors.first;
      return;
    }

    final gone = chat.messages.where((m) => m.topic == id).toList();

    _drop([for (final m in gone) m.blob], replay);

    for (final message in gone) {
      chat.messages.remove(message);
      chat.index.remove(message.ref);
    }

    chat.topics.remove(id);
    chat.unreads.remove(id);

    if (open == chat.id && openTopic == id) {
      openTopic = '';
    }
  }

  void _retracted(Chat chat, Map<String, dynamic> rec, bool replay) {
    final author = _from(chat, rec['peer'] as String? ?? '');
    final target = rec['target'] as int?;
    Message? found;

    for (final message in chat.messages.reversed) {
      if (target != null) {
        if (message.ref == target && message.out) {
          found = message;
          break;
        }
        continue;
      }

      if (!message.image || message.blob.isEmpty) {
        continue;
      }

      if (message.topic != (rec['tp'] as String? ?? '')) {
        continue;
      }

      if (author == '00' ? !message.out : message.author != author) {
        continue;
      }

      if (message.hash == rec['hash']) {
        found = message;
        break;
      }
    }

    if (found == null || found.gone) {
      return;
    }

    found.gone = true;
    _drop([found.blob], replay);
  }

  void _forget(String id) {
    if (chats.remove(id) == null) {
      return;
    }

    if (open == id) {
      open = null;
      openTopic = '';
    }
  }

  void _drop(List<String> files, bool replay) {
    final list = files.where((f) => f.isNotEmpty).toList();

    for (final file in list) {
      _data.remove(file);
    }

    if (replay || list.isEmpty) {
      return;
    }

    unawaited(_invoke<void>('drop', list).catchError((_) {}));
  }

  void _read(String id, String topic) {
    final chat = chats[id];

    if (chat == null) {
      return;
    }

    if (topic.isEmpty) {
      chat.unread = 0;
    } else {
      chat.unreads[topic] = 0;
    }
  }

  Future<void> read(Chat chat, String topic) async {
    if (chat.unreadIn(topic) == 0) {
      return;
    }

    _read(chat.id, topic);
    _changed();

    final what = <String, Object>{'what': 'read', 'chat': chat.id};

    if (topic.isNotEmpty) {
      what['topic'] = topic;
    }

    await _ask(what);
  }

  Future<void> select(Chat? chat, String topic) async {
    final was = open;

    open = chat?.id;
    openTopic = topic;
    _changed();

    if (was != null && was != chat?.id && !Chat(was).isGroup) {
      await _invoke<void>('open', {
        'flock': flockOf(was),
        'peer': localOf(was),
        'on': false,
      });
    }

    if (chat == null) {
      return;
    }

    if (!chat.isGroup && was != chat.id) {
      await _invoke<void>('open', {
        'flock': chat.flock,
        'peer': localOf(chat.id),
        'on': true,
      });
    }

    if (focused) {
      await read(chat, topic);
    }
  }

  Future<void> focus(bool on) async {
    focused = on;

    final id = open;
    if (!on || id == null) {
      return;
    }

    final chat = chats[id];
    if (chat != null) {
      await read(chat, openTopic);
    }
  }

  void _saw(int ref) {
    if ((ref & companionRef) == 0) {
      return;
    }

    final at = ref & ~companionRef;

    if (at > _lastRef) {
      _lastRef = at;
    }
  }

  int _ref() {
    final now = DateTime.now().microsecondsSinceEpoch;
    _lastRef = now > _lastRef ? now : _lastRef + 1;
    return companionRef | _lastRef;
  }

  Map<String, Object> _target(Chat chat, String topic) {
    final out = <String, Object>{};

    if (topic.isNotEmpty) {
      out['topic'] = topic;
    }

    out['flock'] = chat.flock;

    if (!chat.isGroup) {
      out['peer'] = localOf(chat.id);
      return out;
    }

    out['owner'] = localOf(chat.owner);
    out['group'] = chat.group;
    out['serial'] = chat.serial.toRadixString(16);
    out['members'] = [
      for (final m in chat.members)
        if (m != selfIn(chat.id)) localOf(m),
    ];

    return out;
  }

  void _pending(
    Chat chat,
    int ref,
    String topic, {
    String text = '',
    String kind = '',
    String file = '',
    String name = '',
    int size = 0,
    String author = '',
    String hash = '',
  }) {
    final message = Message(
      chat: chat.id,
      ref: ref,
      out: true,
      author: '00',
      at: DateTime.now().millisecondsSinceEpoch,
      text: text,
      kind: kind,
      blob: file,
      name: name,
      size: size,
      quoteAuthor: author,
      quoteHash: hash,
      topic: topic,
    );

    if (chat.isGroup) {
      message.to.addAll([
        for (final m in chat.members)
          if (m != selfIn(chat.id)) localOf(m),
      ]);
    }

    _sending.add('${chat.id}:$ref');
    chat._add(message);
    _changed();
  }

  void _queued(Map<String, dynamic> event) {
    final group = event['group'] as String?;
    final flock = event['flock'] as String? ?? '';
    final id = group == null
        ? '$flock-${event['peer']}'
        : '$flock-g${event['owner']}$group';
    final chat = chats[id];
    final ref = event['ref'] as int;
    final kind = event['kind'] as String? ?? '';

    _saw(ref);

    if (chat == null || chat.index.containsKey(ref)) {
      return;
    }

    var file = '';

    if (kind.isNotEmpty) {
      file = '$id-${ref.toRadixString(16).padLeft(16, '0')}.dat';
    }

    _pending(
      chat,
      ref,
      event['topic'] as String? ?? '',
      text: event['text'] as String? ?? '',
      kind: kind,
      file: file,
      name: event['name'] as String? ?? '',
      size: event['size'] as int? ?? 0,
      author: event['author'] as String? ?? '',
      hash: event['hash'] as String? ?? '',
    );
  }

  void _unsent(String id, int ref) {
    if (!_sending.remove('$id:$ref')) {
      return;
    }

    final message = chats[id]?.index[ref];

    chats[id]?._remove(ref);

    if (message != null && message.blob.isNotEmpty) {
      _drop([message.blob], false);
    }
  }

  Future<void> send(
    Chat chat,
    String text, {
    String topic = '',
    Message? reply,
  }) async {
    final ref = _ref();
    final args = <String, Object>{
      ..._target(chat, topic),
      'ref': ref.toRadixString(16),
      'text': text,
    };

    String hash = '';

    if (reply != null) {
      hash = await digest(reply);
      args['author'] = mine(reply) ? '00' : localOf(reply.author);
      args['hash'] = hash;
    }

    await _invoke<void>('send', args);

    _pending(
      chat,
      ref,
      topic,
      text: text,
      author: args['author'] as String? ?? '',
      hash: hash,
    );
  }

  Future<void> sendFile(
    Chat chat, {
    required bool image,
    String topic = '',
  }) async {
    final ref = _ref();
    final kind = image ? 'image' : 'file';
    final out = await _invoke<Map<Object?, Object?>>('sendFile', {
      ..._target(chat, topic),
      'ref': ref.toRadixString(16),
      'kind': kind,
    });

    if (out == null) {
      return;
    }

    await _sentData(chat, ref, kind, topic, out);
  }

  Future<bool> pasteImage(Chat chat, {String topic = ''}) async {
    final png = await _invoke<Uint8List>('clipboard');

    if (png == null) {
      return false;
    }

    final ref = _ref();
    final out = await _invoke<Map<Object?, Object?>>('sendData', {
      ..._target(chat, topic),
      'ref': ref.toRadixString(16),
      'kind': 'image',
      'name': 'image.png',
      'data': png,
    });

    await _sentData(chat, ref, 'image', topic, out!);

    return true;
  }

  Future<void> memoRecord() => _invoke<void>('memoRecord');

  Future<void> memoCancel() => _invoke<void>('memoCancel');

  Future<void> memoSend(Chat chat, {String topic = ''}) async {
    final ref = _ref();
    final out = await _invoke<Map<Object?, Object?>>('memoSend', {
      ..._target(chat, topic),
      'ref': ref.toRadixString(16),
    });

    await _sentData(chat, ref, 'voice', topic, out!);
  }

  Future<void> memoPlay(Message message, {int at = 0, double rate = 1}) =>
      _invoke<void>('memoPlay', {
        'file': message.file,
        'at': at,
        'rate': rate,
      });

  Future<void> memoStop() => _invoke<void>('memoStop');

  Future<void> memoSeek(int at) => _invoke<void>('memoSeek', at);

  Future<void> memoSpeed(double rate) => _invoke<void>('memoSpeed', rate);

  Future<(int, int)?> memoPosition() async {
    final list = await _invoke<List<Object?>>('memoPosition');

    if (list == null) {
      return null;
    }

    return (list[0] as int, list[1] as int);
  }

  Future<void> _sentData(
    Chat chat,
    int ref,
    String kind,
    String topic,
    Map<Object?, Object?> out,
  ) async {
    final file = out['file'] as String;

    _pending(
      chat,
      ref,
      topic,
      kind: kind,
      file: file,
      name: out['name'] as String,
      size: out['size'] as int,
    );
  }

  Future<void> typing(Chat chat, String topic, bool on) async {
    final args = <String, Object>{..._target(chat, topic), 'on': on};

    await _invoke<void>('typing', args);
  }

  Future<void> _ask(Map<String, Object> what) =>
      _invoke<void>('ask', jsonEncode({...what, 'id': _ref()}));

  Future<void> topic(Chat chat, String name, int color, {String id = ''}) {
    final what = <String, Object>{
      'what': 'topic',
      'chat': chat.id,
      'name': name,
      'color': color,
    };

    if (id.isNotEmpty) {
      what['topic'] = id;
    }

    return _ask(what);
  }

  Future<void> removeTopic(Chat chat, String id) => _ask({
    'what': 'topic',
    'chat': chat.id,
    'topic': id,
    'name': '',
    'color': 0,
  });

  Future<void> createGroup(String flock, String name, List<String> members) =>
      _ask({
        'what': 'group',
        'flock': flock,
        'name': name,
        'members': members,
      });

  Future<void> editGroup(Chat chat, String name, List<String> members) =>
      _ask({
        'what': 'edit',
        'chat': chat.id,
        'name': name,
        'members': members,
      });

  Future<void> leaveGroup(Chat chat) =>
      _ask({'what': 'leave', 'chat': chat.id});

  Future<void> retract(Chat chat, Message message) async {
    final hashes = await _invoke<List<Object?>>('hash', [message.file]);

    await _ask({
      'what': 'retract',
      'chat': chat.id,
      'ref': message.ref,
      'hash': hashes?.first as String? ?? '',
      'topic': message.topic,
    });
  }

  Future<bool> avatar() async => await _invoke<bool>('avatar', true) ?? false;

  Future<void> dropAvatar() => _invoke<void>('avatar', false);

  Future<void> favorite(Message message, bool on) => _ask({
    'what': 'favorite',
    'chat': message.chat,
    'ref': message.ref,
    'on': on,
  });

  Future<void> pin(Message message, bool on) => _ask({
    'what': 'pin',
    'chat': message.chat,
    'ref': message.ref,
    'on': on,
  });

  Future<void> star(Chat chat, bool on) =>
      _ask({'what': 'star', 'chat': chat.id, 'on': on});

  Future<void> mute(Chat chat, int until) =>
      _ask({'what': 'mute', 'chat': chat.id, 'until': until});

  Future<void> delete(Chat chat) =>
      _ask({'what': 'delete', 'chat': chat.id});

  Future<void> dial(String peer) =>
      _invoke<void>('call', _call('dial', peer));

  Future<void> answer(String peer) =>
      _invoke<void>('call', _call('answer', peer));

  Future<void> decline(String peer) =>
      _invoke<void>('call', _call('decline', peer));

  Future<void> hangup(String peer) =>
      _invoke<void>('call', _call('hangup', peer));

  Future<void> muteCall(bool on, String peer) =>
      _invoke<void>('call', _call(on ? 'mute' : 'unmute', peer));

  Map<String, String> _call(String op, String peer) => {
    'op': op,
    'flock': flockOf(peer),
    'peer': localOf(peer),
  };

  List<Message> get favorites => [
    for (final chat in chats.values)
      for (final m in chat.messages)
        if (m.favorite) m,
  ]..sort((a, b) => b.at.compareTo(a.at));

  Chat? chatOf(Message message) => chats[message.chat];

  Future<String> digest(Message message) async {
    final known = message.digest;
    if (known != null) {
      return known;
    }

    final list = await _invoke<List<Object?>>('digest', [
      '${message.media}\n${message.content}',
    ]);

    message.digest = list!.first as String;
    return message.digest!;
  }

  Future<void> digests(Chat chat) async {
    final want = chat.messages
        .where((m) => m.digest == null && m.system.isEmpty)
        .toList();
    if (want.isEmpty) {
      return;
    }

    final list = await _invoke<List<Object?>>(
      'digest',
      want.map((m) => '${m.media}\n${m.content}').toList(),
    );

    for (var i = 0; i < want.length; i++) {
      want[i].digest = list![i] as String;
    }

    _changed();
  }

  Message? quoted(Chat chat, Message message) {
    if (message.quoteHash.isEmpty) {
      return null;
    }

    final at = chat.messages.indexOf(message);
    final self = message.quoteAuthor == '00' ||
        message.quoteAuthor == localOf(selfIn(chat.id));

    for (var i = at - 1; i >= 0; i--) {
      final other = chat.messages[i];

      if (other.topic != message.topic) {
        continue;
      }

      if (other.system.isNotEmpty) {
        continue;
      }

      if (self != mine(other)) {
        continue;
      }

      if (!self && localOf(other.author) != message.quoteAuthor) {
        continue;
      }

      if (other.digest == message.quoteHash) {
        return other;
      }
    }

    return null;
  }

  Future<Uint8List?> data(String file) {
    if (file.isEmpty) {
      return Future.value(null);
    }

    return _data.putIfAbsent(file, () => _fetch(file));
  }

  Future<Uint8List?> _fetch(String file) async {
    try {
      return await _invoke<Uint8List>('data', file);
    } on DeskError {
      _data.remove(file);
      return null;
    }
  }

  Future<Uint8List?> avatarOf(String peer) {
    if (isSelf(peer)) {
      return data(selfAvatar);
    }

    return data(avatars[peer] ?? '');
  }

  Future<void> url(String link) => _invoke<void>('url', link);

  Future<void> save(Message message) async {
    await _invoke<bool>('save', {'file': message.file, 'name': message.name});
  }
}
