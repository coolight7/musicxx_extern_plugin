import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'events.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// 主程序动作处理器：接收插件参数，返回结果（JSON 可序列化）
typedef MusicxxPluginActionHandler =
    Object? Function(MusicxxPluginActionInvocation invocation);

/// 一次动作请求（插件 → Dart）
class MusicxxPluginActionInvocation {
  const MusicxxPluginActionInvocation({
    required this.requestId,
    required this.plugin,
    required this.action,
    required this.args,
    required this.extendDeadline,
  });

  /// 请求 id（回复时原样带回）
  final int requestId;

  /// 发起插件（可能是 `js:<id>` 的 id 形式）
  final String plugin;

  /// 动作全名（`musicxx.<域>.<动作>`）
  final String action;

  /// 参数
  final Map<String, Object?> args;

  /// 延长本次请求的超时（长任务用；主程序下限 1 s）—— 需由主程序确认，v1 仅记录
  final void Function(Duration extra) extendDeadline;

  @override
  String toString() =>
      'MusicxxPluginActionInvocation($plugin → $action #$requestId)';
}

/// 主程序动作注册表
///
/// 插件经 `musicxx.host.request_action` 发起动作；Dart 侧在 [register] 注册实现。
/// **未注册的动作会立即收到"未找到"**，避免插件 op 一直挂到超时。
class MusicxxPluginActions {
  MusicxxPluginActions.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  final Map<String, MusicxxPluginActionHandler> _handlers =
      <String, MusicxxPluginActionHandler>{};

  /// 已注册动作名
  Set<String> get registeredActions => Set<String>.unmodifiable(_handlers.keys);

  /// 还没回复的异步动作数量（异步处理器返回 Future 时记一笔）
  ///
  /// 给"兜底轮询要不要跑"用：这些回复还在等结果，事件回传不能漏（见 runtime 的定时器）。
  int get pendingResponses => _pendingResponses;
  int _pendingResponses = 0;

  /// 注册动作实现（同名覆盖）
  void register(String action, MusicxxPluginActionHandler handler) {
    _handlers[action] = handler;
  }

  /// 注销动作
  void unregister(String action) {
    _handlers.remove(action);
  }

  /// 主动回复某个请求（一般由内部调用；长任务可先记录 requestId 再异步回复）
  void respond(int requestId, {Object? result, int status = 0, String? error}) {
    if (!_runtime.isRunning) {
      return;
    }
    final String payload = result == null
        ? (error == null ? '' : _errorJson(error))
        : _encode(result);
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_action_respond(
        _runtime.host,
        requestId,
        status,
        arena.view(payload),
        log,
      );
      if (rc != 0) {
        _runtime.log(3, 'action_respond(#$requestId) 失败(code=$rc)');
      }
    } finally {
      arena.dispose();
    }
  }

  /// 直接执行一个已注册的主程序动作（**主程序 UI 触发**；不经插件 op 通道）
  ///
  /// 用途：插件用声明式 UI 声明 `{"kind":"action","name":"musicxx.player.toggle"}`，
  /// 用户点击后由主程序直接调用该动作实现（与插件发起动作走同一份实现与权限规则）。
  /// - `pluginId`：调用上下文（权限判定与插件私有存储定位用；可为空）；
  /// - 返回动作结果；动作未注册返回 `null`（调用方可据此提示）。
  Future<Object?> invoke(
    String action, {
    String pluginId = '',
    Map<String, Object?> args = const <String, Object?>{},
  }) async {
    final MusicxxPluginActionHandler? handler = _handlers[action];
    if (handler == null) {
      return null;
    }
    final MusicxxPluginActionInvocation invocation =
        MusicxxPluginActionInvocation(
          requestId: 0,
          plugin: pluginId,
          action: action,
          args: args,
          extendDeadline: (Duration _) {},
        );
    try {
      final result = handler(invocation);
      if (result is Future) {
        return await result;
      }
      return result;
    } catch (error) {
      return <String, Object?>{'ok': false, 'error': error.toString()};
    }
  }

  // ==================== 内部 ====================

  /// `musicxx.action.request` 事件 → 同步调用 Dart 处理器并回复
  ///
  /// 说明：v1 在事件泵里同步执行处理器（处理器应快速返回；耗时工作请自行异步化并在完成后
  /// 调用 [respond]）。这样插件侧 op 能在第一时间拿到结果，而不是等超时。
  void handleActionRequest(MusicxxPluginEvent event) {
    final int requestId = event.intOf('requestId') ?? 0;
    if (requestId == 0) {
      return;
    }
    final String action = event.stringOf('action') ?? '';
    final rawArgs = event.payload['args'];
    final Map<String, Object?> args = rawArgs is Map<String, Object?>
        ? rawArgs
        : (rawArgs is Map<Object?, Object?>
              ? rawArgs.cast<String, Object?>()
              : const <String, Object?>{});
    final MusicxxPluginActionHandler? handler = _handlers[action];
    if (handler == null) {
      respond(requestId, status: -4, error: 'action_not_registered: $action');
      return;
    }
    final MusicxxPluginActionInvocation invocation =
        MusicxxPluginActionInvocation(
          requestId: requestId,
          plugin: event.plugin,
          action: action,
          args: args,
          extendDeadline: (Duration _) {},
        );
    try {
      final result = handler(invocation);
      if (result is Future) {
        // 异步处理器：先说明"已受理"，完成后再由处理器自行 respond（或返回未来值）
        ++_pendingResponses;
        result.then<void>(
          (Object? value) {
            --_pendingResponses;
            respond(requestId, result: value);
          },
          onError: (Object error) {
            --_pendingResponses;
            respond(requestId, status: -99, error: error.toString());
          },
        );
        return;
      }
      respond(requestId, result: result);
    } catch (error) {
      respond(requestId, status: -99, error: error.toString());
    }
  }

  /// `musicxx.action.cancel`（插件取消 / 主程序超时）：通知已注册的处理器
  void handleActionCancel(MusicxxPluginEvent event) {
    // v1 只记一条日志；需要中断的处理器的实现可以订阅事件流自行收尾（取消令牌尚未接入）
    _runtime.log(
      3,
      '动作请求被取消: #${event.intOf('requestId')} (${event.stringOf('reason') ?? ''})',
    );
  }

  /// 主程序收尾：没回复完的异步动作不再有人接，计数归零
  void handleDisposed() {
    _pendingResponses = 0;
  }

  static String _encode(Object? value) => jsonEncode(value);

  static String _errorJson(String message) =>
      jsonEncode(<String, Object?>{'error': message});
}

/// 动作名常量（主程序在 Store 里注册对应实现；这里只列名字）
abstract final class MusicxxPluginActionNames {
  /// 切到前台（把主窗口显示出来）
  ///
  /// 与命令行 `window.foreground` 是同一份实现（`GlobalStore.appForeground`）。
  /// 切完会派发通知型钩子 `musicxx.app.foreground`，并刷新状态镜像 `musicxx.state.app`。
  static const String appForeground = 'musicxx.app.foreground';

  /// 切到后台（隐藏主窗口，播放不受影响）
  ///
  /// 与命令行 `window.background` 是同一份实现（`GlobalStore.appBackground`）。
  /// 切完会派发通知型钩子 `musicxx.app.background`，并刷新状态镜像 `musicxx.state.app`。
  static const String appBackground = 'musicxx.app.background';

  static const String playerPlay = 'musicxx.player.play';
  static const String playerPause = 'musicxx.player.pause';
  static const String playerToggle = 'musicxx.player.toggle';
  static const String playerStop = 'musicxx.player.stop';
  static const String playerNext = 'musicxx.player.next';
  static const String playerPrev = 'musicxx.player.prev';
  /// 按来源播放（本地路径 / `content://` / 文件链接 / 拟声音源字符串）。
  /// 和 applink（`musicxx://play/`）、启动参数、命令行、MCP 是同一个入口，
  /// 参数名也一致：`src` / `sources` / `type` / `name`。
  static const String playerPlaySrc = 'musicxx.player.playSrc';
  static const String playerSeek = 'musicxx.player.seek';
  static const String playerSetVolume = 'musicxx.player.setVolume';
  static const String playerSetSpeed = 'musicxx.player.setSpeed';
  static const String playerSetPitch = 'musicxx.player.setPitch';
  static const String playerSetQuality = 'musicxx.player.setQuality';
  static const String playerSetLoopMode = 'musicxx.player.setLoopMode';
  static const String playerSetMediaType = 'musicxx.player.setMediaType';

  // ==================== 歌单（playlist = 单张歌单） ====================
  //
  // 三个词含义固定，不再混用（与 docs/plugin-api.md 的术语表一致）：
  // * `playlist` —— 单张歌单（`pid` / `name` / `songs` / `songNum`）；
  // * `playlists` —— 多张歌单的集合（我的 / 我喜欢 / 本地）；
  // * `songlist` —— 播放列表，也就是当前正在播的那一串歌。

  /// 列出歌单：我的 / 我喜欢 / 本地
  ///
  /// 参数：无。
  /// 返回：`{mine, favorite, local, currentPid, currentName}`；歌单项是
  /// `{pid, name, count, songNum, listen, type}`（`count` = 已加载的曲目数，`songNum` = 歌单声明的
  /// 曲目数）。`currentPid` / `currentName` 是**当前播放列表**的 pid 与名字，方便接着调
  /// [songlistQuerySongs] 或 [playlistQuerySongs]。
  static const String playlistQueryLists = 'musicxx.playlist.queryLists';

  /// 读一张歌单里的歌
  ///
  /// 参数：`pid`（必填）、`type?`（`local` = 本地歌单，其它值按在线歌单，用于歌单还没进
  /// 当前账号的歌单表时按 `pid` 直接查）、`limit?`（缺省 200）、`offset?`（缺省 0）。
  /// 返回：`{pid, total, songs:[{sid, name, artist, album, durationMs}]}`。
  ///
  /// 不给 `pid` 时读的是当前播放列表（老行为，留着不为难老插件）—— 新代码要读当前播放列表请用
  /// [songlistQuerySongs]，读的是哪一份一眼就能看出来。
  static const String playlistQuerySongs = 'musicxx.playlist.querySongs';

  /// 播放一张歌单
  ///
  /// 参数：`pid`（必填）、`index?`（第几首，从 0 开始；越界或缺省按 0）。
  /// 返回：`{ok:true}`；歌单为空或加载失败是 `{ok:false, error}`。
  static const String playlistPlay = 'musicxx.playlist.play';

  /// 把歌曲加进**一张歌单**（`playlist` = 单张歌单，不是播放列表）
  ///
  /// 参数：`pid`（必填）、`dedup`（缺省 true）、`songs`（首数不限，或单首写成 `song`）。
  /// 每首歌：`type`（插件自己的音源类型 `plugin.<插件id>.<类型名>`）、`src`、`name?` /
  /// `artist?`（空则按 `fileName` 交给主程序识别）、`fileName?`、`size?`、`durationMs?`、
  /// `cover?`（本地路径或 `http(s)` 地址）。
  ///
  /// 返回：`{ok, pid, name, total, added, skipped}`；失败是 `{ok:false, error}`。
  static const String playlistAddSong = 'musicxx.playlist.addSong';

  /// 从一张歌单里移除歌曲
  ///
  /// 参数：`pid`（必填）、`sid`（一首）或 `sids`（多首）。
  /// 返回：`{ok:true, pid, name, total, removed, removedSids, notFound}`
  /// （`notFound` = 歌单里没找到的那几个 sid，不算失败）。
  /// 只改保存下来的歌单，**不会**动到正在播放的队列；要改队列用 `musicxx.songlist.removeSong`。
  static const String playlistRemoveSong = 'musicxx.playlist.removeSong';

  /// 新建一张歌单
  ///
  /// 参数：`name`（必填，1 ~ 50 个字符）、`type?`（`local` 缺省 = 本地歌单；`net` = 在线歌单，
  /// 需要登录）、`songs?`（建完顺手加进去，格式与 [playlistAddSong] 的 `songs` 一样）。
  /// 返回：`{ok:true, pid, name, type, total}`。
  static const String playlistCreate = 'musicxx.playlist.create';

  /// 改一张歌单里某首歌的信息（歌名 / 歌手 / 专辑 / 音源等）
  ///
  /// 参数：`pid`、`sid`（必填，定位到哪一张歌单里的哪一首），再加要改的字段：
  /// `name` / `artist` / `album` / `track` / `year` / `genre` / `durationMs` /
  /// `src` / `type`（音源类型）/ `cover`（同 [playlistAddSong] 的 `cover`）。
  /// 没给的字段保持原值；`sid` 本身不能改。
  /// 返回：`{ok:true, pid, sid, song:{…}}`。
  static const String playlistUpdateSong = 'musicxx.playlist.updateSong';

  /// 同步歌单音源：把歌单绑定的音源重新读一遍，新增的歌曲加进歌单
  ///
  /// 参数：`pid`（必填，目标歌单：本地歌单 / 自己的在线歌单 / “我喜欢”）。
  /// 歌单绑定了哪些音源（本地文件夹、WebDAV、云盘、Bili 收藏夹、媒体库…）只能在应用界面里配，
  /// 这里只按已经绑好的音源同步，不收 `sources`。
  ///
  /// 返回：`{ok, pid, name, total, added, cancelled, failedSources, elapsedMs, sources:[…]}`；
  /// `sources` 每项 `{index, id, type, name, loaded, added, error?}`（`loaded` = 这条音源读到多少首，
  /// `added` = 这一条新加进歌单多少首）。失败是 `{ok:false, error}`：未找到歌单 / 歌单无法加载 /
  /// 没有修改权限 / 没有绑定音源 / 每条音源都读取失败 / 保存失败（已回滚）/ 这个歌单正在同步。
  ///
  /// 不是用户从应用界面点「开始同步音源」触发的（插件 / MCP / 命令行），应用的信息栏会显示一条提示：
  /// 同步成功后 30 秒自动移除，失败则保留（可以点开看每一条音源的结果）。
  static const String playlistSyncSongsSync = 'musicxx.playlist.syncSongs.sync';

  // ==================== 播放列表（songlist = 当前播放队列） ====================

  /// 读当前播放列表（正在播的那一串歌）里的歌
  ///
  /// 参数：`{limit?=200, offset?=0}`。
  /// 返回：`{pid, total, songs:[{sid, name, artist, album, durationMs}], available}`
  /// （`available` 说明当前播放列表还能不能用）。
  static const String songlistQuerySongs = 'musicxx.songlist.querySongs';

  /// 播当前播放列表里的某一首
  ///
  /// 参数：`{sid}`，**这首歌必须已经在当前播放列表里**，否则 `{ok:false, error}`。
  static const String songlistPlaySong = 'musicxx.songlist.playSong';

  /// 往当前播放列表里加歌
  ///
  /// 参数与 [playlistAddSong] 的歌曲格式一样：`songs`（或单首 `song`）、`dedup?`（缺省 true），
  /// 另可给 `afterSid?` 插到那一首之后（不给就插到最前，正在播的歌不会因此换掉）。
  /// 返回：`{ok:true, pid, name, total, added, skipped}`。
  static const String songlistAddSong = 'musicxx.songlist.addSong';

  /// 从当前播放列表里移除歌曲
  ///
  /// 参数：`sid`（一首）或 `sids`（多首）。
  /// 返回：`{ok:true, total, removed, removedSids, notFound}`。
  /// 只改队列，不写回任何歌单；移除正在播的那一首不会停下来（与界面上的“从播放列表移除”一致）。
  static const String songlistRemoveSong = 'musicxx.songlist.removeSong';

  /// 改当前播放列表里某首歌的信息
  ///
  /// 参数与 [playlistUpdateSong] 一样，但用当前队列定位（不需要 `pid`）：`sid` + 要改的字段。
  /// 正在播的那一首改完会立即刷新播放信息（名字 / 歌手 / 封面）。
  /// 返回：`{ok:true, sid, song:{…}}`。
  static const String songlistUpdateSong = 'musicxx.songlist.updateSong';

  // ==================== 过时的分类 musicxx.library.* ====================
  //
  // 这些名字**保留可用**（主程序仍然把它们注册成同一份实现），但新代码不要再写：
  // 一个前缀里混着“歌单”和“播放列表”两个概念，调用方看不出自己在操作哪一个。
  // 新名字见上面两组；从未实现的那几条不登记别名，调它们只会得到“动作未注册”。

  /// 过时的分类 `musicxx.library.*` 里**仍然可用**的名字 → 现在的动作名
  ///
  /// 主程序按这张表把旧名字注册成同一份实现，命令行与 `/cli/list` 也按它标出“过时”。
  /// 键写成字符串字面量而不是引用下面的 `@Deprecated` 常量：在自己包里用它们会触发弃用告警。
  static const Map<String, String> deprecatedLibraryAliases = <String, String>{
    'musicxx.library.querySonglists': playlistQueryLists,
    'musicxx.library.querySongs': playlistQuerySongs,
    'musicxx.library.playSong': songlistPlaySong,
    'musicxx.library.playSonglist': playlistPlay,
  };

  @Deprecated(
    'musicxx.library 分类已过时：用 musicxx.playlist.queryLists（列出歌单）',
  )
  static const String libraryQuerySonglists = 'musicxx.library.querySonglists';

  @Deprecated(
    'musicxx.library 分类已过时：读一张歌单用 musicxx.playlist.querySongs，'
    '读当前播放列表用 musicxx.songlist.querySongs',
  )
  static const String libraryQuerySongs = 'musicxx.library.querySongs';

  @Deprecated(
    'musicxx.library 分类已过时：用 musicxx.playlist.querySongs（带 pid）',
  )
  static const String libraryQuerySonglist = 'musicxx.library.querySonglist';

  @Deprecated(
    'musicxx.library 分类已过时：用 musicxx.songlist.playSong（播当前播放列表里的某一首）',
  )
  static const String libraryPlaySong = 'musicxx.library.playSong';

  @Deprecated('musicxx.library 分类已过时：用 musicxx.playlist.play（播放一张歌单）')
  static const String libraryPlaySonglist = 'musicxx.library.playSonglist';

  @Deprecated('musicxx.library 分类已过时：用 musicxx.playlist.removeSong')
  static const String libraryRemoveSong = 'musicxx.library.removeSong';

  @Deprecated('musicxx.library 分类已过时：用 musicxx.playlist.create')
  static const String libraryCreateSonglist = 'musicxx.library.createSonglist';

  @Deprecated('musicxx.library 分类已过时：用 musicxx.playlist.updateSong')
  static const String librarySetSongInfo = 'musicxx.library.setSongInfo';

  /// 站内搜索（**当前版本没实现**）
  ///
  /// 没有新名字：曲库检索既不是“歌单”也不是“播放列表”的操作，等真正实现时再定分类。
  @Deprecated('musicxx.library 分类已过时；这一条从未实现，也没有替代的名字')
  static const String librarySearch = 'musicxx.library.search';

  // ==================== 歌词（单数域 lyric） ====================
  //
  // 名字与钩子 / 变量 / 状态镜像一致，统一用 `musicxx.lyric.*`。
  // 旧的复数域 `musicxx.lyrics.*` 已删除（不保留别名），从未实现过的 `sync` 连名字一起删掉。

  /// 当前歌曲的歌词信息和内容
  ///
  /// 参数：`limit?`（折叠后的行数上限，缺省 0 = 全部）、`word?`（true 时每行附带逐字时间）。
  /// 返回：`{ok, hasLyric, sid, songName, songArtist, lrcid, name, artist, sourceType,
  /// timeType, lineCount, index, rawIndex, offsetMs, songOffsetMs, globalOffsetMs,
  /// hasTranslate, showTranslate, lines}`。
  ///
  /// `lines` 里一句 = 一行原文 + 同组的全部译文行（`translate`），`index` 就是 `lines` 的下标；
  /// `timeMs` 是毫秒，没有时间戳的行是 `null`。
  static const String lyricCurrentGet = 'musicxx.lyric.current.get';

  /// 改当前歌曲的拟声云歌词与这首歌的歌词时间偏移
  ///
  /// 参数：`lrcid?`（拟声云歌词 id，只能是云歌词）、`offsetMs?`（毫秒，正 = 歌词提前）。
  /// 两者至少给一个。只改正在播放的这一首：换歌词按界面"选择歌词"的同一份实现落绑定，
  /// 偏移写进这首歌的歌词绑定（不是全局偏移设置）。
  /// 返回：`{ok, sid, lrcid, songOffsetMs, offsetMs, sourceType, lineCount, persisted}`。
  static const String lyricCurrentUpdate = 'musicxx.lyric.current.update';

  /// 用一段 LRC 文本设置当前歌词（只管显示）
  ///
  /// 参数：`lrc`（标准或增强型 LRC 文本）。**不写缓存、不上传、不改歌曲绑定的歌词**，
  /// 切歌或重新加载歌词后恢复成绑定来源的歌词。
  /// 返回：`{ok, sid, lineCount, timeType, hasTranslate}`。
  static const String lyricCurrentUpdateContent =
      'musicxx.lyric.current.updateContent';

  /// 按歌词源读取歌词内容（不要求是当前歌曲）
  ///
  /// 参数：`lrcid?`，或 `type?` + `src?`（`myid` / `local` / `link`），
  /// 另可给 `refresh?`（云歌词跳过内容缓存）、`limit?`、`word?`。
  /// 返回与 [lyricCurrentGet] 同一套行格式：`{ok, found, lrcid, sourceType, name, artist,
  /// timeType, lineCount, lines}`；`found:false` 表示没读到，不是错误。
  static const String lyricGetBySrc = 'musicxx.lyric.getBySrc';

  /// 搜索拟声云歌词
  ///
  /// 参数：`keyword?`（也接受 `info` / `q`），或 `name` + `artist`（按应用的识别规则生成搜索串），
  /// `limit?`（缺省 20，最大 50）。
  /// 返回：`{ok, keyword, count, lyrics:[{lrcid, name, artist, depict, timeType, timeTypeName,
  /// love, used}]}`；搜索结果只有元信息，内容用 [lyricGetBySrc]。
  static const String lyricSearch = 'musicxx.lyric.search';

  /// 轻提示（应用界面上的一条提示）
  ///
  /// 参数：`text`（也接受 `message`）；`kind` = `error` 时走错误样式。
  /// **固定记一条插件日志**（原来的 `musicxx.ui.notify` 已合并到这里）。
  static const String uiToast = 'musicxx.ui.toast';
  static const String uiDialog = 'musicxx.ui.dialog';

  /// 打开一层浮层（sheet），等用户关掉后返回结果
  ///
  /// 参数：`preset`（`custom` 缺省 / `picker`）、`id`、`title`、`depict`、`blocks`（`custom` 的内容）、
  /// `options`（`picker` 的候选项）、`value`（`picker` 的当前值）、`textConfirm` / `textCancel`、
  /// `size`（尺寸预设）、`dismissible`（缺省 true）。
  ///
  /// 返回：`{ok, reason, value?}`（`reason` 是 `confirmed` / `cancelled` / `closed`）；
  /// `preset: "picker"` 时是 `{ok, selected}` 或 `{ok, cancelled: true}`。
  static const String uiOverlayShow = 'musicxx.ui.overlay.show';

  /// 收起这个插件当前那一层浮层（参数 `id` 可选：只关名字对得上的那一层）
  ///
  /// 返回 `{ok, closed}`；没有开着的浮层时 `closed` 为 false。
  static const String uiOverlayClose = 'musicxx.ui.overlay.close';

  /// 打开一层“插件视图”浮层（底部弹层），等用户关掉后返回
  ///
  /// 与 `musicxx.ui.overlay.show` 的区别：`show` 的内容是调用方给的 `blocks`（静态描述），
  /// 这一条的内容是**插件自己的视图**，所以能在浮层里翻目录 / 改选择（浮层里的动作返回
  /// `{"view": …}` 就刷新内容）。
  ///
  /// 参数：`route`（必填，`ext://<插件id>/<视图id>`）、`title?`、`arguments?`（原样交给插件能力；
  /// 不给时把除 `route` / `title` / `size` / `dismissible` / `textConfirm` / `textCancel`
  /// 之外的键当参数）。
  /// 返回：`{ok, reason, title?}`（`reason` 是 `confirmed` / `cancelled` / `closed`）。
  ///
  /// 动作**在浮层关掉之后才完成**：调用方正好能在关闭后刷新自己的列表。
  /// 插件在能力处理器里调它时**不要 await**（会与主程序动作互相等）。
  static const String uiOpenOverlaySheet = 'musicxx.ui.openOverlaySheet';
  static const String uiOpenRoute = 'musicxx.ui.openRoute';

  /// 让主程序打开「选择歌曲」弹窗（挑自己这个音源里的歌，选完直接加进歌单）
  ///
  /// 前提是先声明 `musicxx.source.browse` 并实现 `drive.list` 能力。参数：`type`（必填，
  /// 自己声明的音源类型 id）、`view?`（浏览哪个分类，不给就用第一个）、`title?`、
  /// `path?`（起始目录，缺省用声明里的 `root`）、`pid?`（目标歌单，不给就是当前播放列表）、
  /// `multiple?`（默认 true）、`allowSelectDir?`（默认 true）、`search?`、`pageSize?`。
  /// 返回：`{ok, added, skipped, cancelled, view}`（`view` = 这次浏览的分类）。
  ///
  /// 动作**在弹窗关掉之后才完成**：插件在能力处理器里调它时**不要 await**
  /// （会与主程序动作互相等，与 `musicxx.ui.openOverlaySheet` 同理）。
  static const String uiPickSongs = 'musicxx.ui.pickSongs';

  /// 让主程序打开「选择文件夹」弹窗（上传目标、保存位置这类场景）
  ///
  /// 参数与 [uiPickSongs] 基本相同，`multiple` 缺省 false。
  /// 返回：`{ok, paths, items, cancelled, view}`（`items` 每条是 `{view, path}`）。
  static const String uiPickDir = 'musicxx.ui.pickDir';

  /// 打开某个浏览来源的**整页列表**（与内置网盘页同一套界面）
  ///
  /// 参数：`type`（必填）、`view?`（起始分类，缺省第一个）、`path?`（起始目录）。
  /// 返回：`{ok, reason:"opened", type, view}`，**不等用户关页面**（插件拿这个页面当自己的
  /// 浏览器用，不需要“关掉之后做什么”）；要等用户挑完再用 [uiPickSongs] / [uiPickDir]。
  static const String uiBrowseDrive = 'musicxx.ui.browseDrive';
  static const String uiSetEntryBadge = 'musicxx.ui.setEntryBadge';
  static const String uiSetFontFamily = 'musicxx.ui.setFontFamily';
  static const String uiGetFontFamily = 'musicxx.ui.getFontFamily';
  static const String uiLoadFont = 'musicxx.ui.loadFont';

  static const String storageGet = 'musicxx.storage.get';
  static const String storageSet = 'musicxx.storage.set';
  static const String storageDelete = 'musicxx.storage.delete';
  static const String storageList = 'musicxx.storage.list';

  /// HTTP 请求 / 下载（**插件框架直接回答**，与原生 `iface.net` 的 `http.fetch` /
  /// `http.download` 是同一份实现；主程序不再另写一套）
  static const String netFetch = 'musicxx.net.fetch';
  static const String netDownload = 'musicxx.net.download';

  static const String renderList = 'musicxx.render.list';
  static const String renderCurrent = 'musicxx.render.current';
  static const String renderSelect = 'musicxx.render.select';

  static const String mediaPalette = 'musicxx.media.palette';
  static const String mediaCover = 'musicxx.media.cover';

  /// 写封面取色（与内置分析写入同一批字段，之后 `musicxx.media.palette` 读到的就是它）
  ///
  /// 参数：`main` / `light` / `lightMuted` / `dark` / `darkMuted`（`#rrggbb` 或 `#aarrggbb`），
  /// 可选 `dominant`（数组）。没给的键保持原样。
  static const String mediaPaletteSet = 'musicxx.media.palette.set';

  /// 把插件自己准备的图片绑定成纹理（渲染项用 `image` 声明引用）
  ///
  /// 参数：`key`（本地名字，渲染项里引用它）、`data`（base64）、
  /// `format`（`png` / `jpeg` / `rgba`；`rgba` 需要 `width` / `height`）。
  static const String mediaBindImage = 'musicxx.media.bindImage';

  /// 释放之前绑定的图片纹理（参数 `key`）
  static const String mediaUnbindImage = 'musicxx.media.unbindImage';

  /// 当前音频频谱（内置『音乐动效』插件提取的数据）
  ///
  /// 参数：`bandCount`（1~64，必须整除 256，缺省 16）、`unit`（`normalized` 缺省 /
  /// `db` / `raw`）、`includeBins`（附带当前帧的 256 个频点）。
  /// 没有数据不是错误：结果里用 `status` / `available` / `loading` / `reason` 表达。
  static const String mediaSpectrum = 'musicxx.media.spectrum';

  /// 播放内核信息：版本、能直接写进 `af` / `vf` 的过滤器、解码器、应用认得的音视频后缀
  ///
  /// 参数：`part`（`all` 缺省 / `versions` / `filters` / `decoders` / `formats`，只影响 `text`）、
  /// `refresh`（布尔，可省：重新采集而不是用缓存）。
  ///
  /// 返回 `{ok, kernel, text}`：`kernel` 是完整数据（`mpvVersion` / `ffmpegVersion` /
  /// `audioFilters` / `videoFilters` / `decoders` / `audioExtensions` / `videoExtensions` /
  /// `counts`）；`audioFilters[].name` 就是能写进 mpv `af` 的名字（`builtin` 为 true 的是
  /// mpv 自带的那几个），`decoders[]` 给编解码器与驱动（`hardware` 标出硬件解码）。
  ///
  /// 与 MCP 工具 `GetMediaKernelInfo`、命令行 `kernel.info` 是同一份数据。
  /// **不要按上游 ffmpeg 文档假设**：应用内嵌的是裁剪过的 ffmpeg，能用的过滤器与解码器
  /// 以这里返回的为准。
  static const String mediaKernel = 'musicxx.media.kernel';

  /// 应用能力导出（见 manager/featureExport/）：
  /// - `list` 列出全部能力的声明（id/标题/说明/参数/输出形态）；
  /// - `call` 调一个能力（`{id, args}`），与 MCP 工具共用同一份实现。
  static const String featureList = 'musicxx.feature.list';
  static const String featureCall = 'musicxx.feature.call';

  /// 列出功能埋点与各自的实现（谁在提供这个功能、排第几、谁把功能关掉了）
  ///
  /// 与命令行 `featurePoint.list`、MCP 工具 `GetFeatureProviders` 共用同一份自省数据。
  /// 参数：`id`（只看一个埋点）、`provided`（只看有实现的埋点）。
  static const String featurePointList = 'musicxx.feature.point.list';

  /// 调一个功能点并拿到结果（应用留出的埋点，或插件自己声明的功能点）
  ///
  /// 参数：
  /// - `id`：功能点 id（应用埋点如 `musicxx.media.spectrum.extract`，
  ///   插件点如 `plugin.<插件id>.<名字>`）；
  /// - `args`：给这个功能点的参数（对象，可省；每个功能点的参数见 `list` 里的 `callDoc`）；
  /// - `refresh`：布尔，可省。true = 跳过值仓重新算一遍。
  ///
  /// 返回 `{ok, id, value, by, identity, fromCache, ms}`；失败是
  /// `{ok:false, error, message}`，`error` 取 `not_callable` / `bad_args` / `no_impl` /
  /// `disabled` / `busy` / `failed`。
  ///
  /// **调用零副作用**：只读值仓、不写值仓、不改应用状态 —— 插件调一次封面取色
  /// 不会换掉用户看到的背景色（要改观感请看 `musicxx.media.palette.set`）。
  /// 同一功能点 + 同一调用方的上一次调用没结束时返回 `busy`（不排队）。
  static const String featurePointCall = 'musicxx.feature.point.call';

  /// 声明一个**自己的**功能点（id 必须形如 `plugin.<自己的插件 id>.<名字>`）
  ///
  /// 插件声明的功能点共用**一条**通用通道钩子（`musicxx.feature.point.call`），而不是每个点
  /// 各占一个钩子 id（插件框架只认固定的钩子表）。声明只描述"本插件提供什么功能"：
  /// 参数 `id`（必填）、`title`（展示名）、`depict`（一句话说明）、`args`（参数说明）、
  /// `result`（结果说明）。同一 id 重复声明 = 覆盖（最后一次生效）。
  /// 声明跟着插件生命周期走（禁用 / 卸载自动撤销），不持久化；重新启用时脚本会再声明一遍。
  /// 声明之后，别的插件与应用都能用 `musicxx.feature.point.call` 调它（`id` 就是这个点）。
  static const String featurePointDefine = 'musicxx.feature.point.define';

  /// 收回自己声明的功能点（参数 `id`）
  ///
  /// 一般不用调：插件被禁用 / 卸载时插件框架会撤销它的全部声明。
  static const String featurePointUndefine = 'musicxx.feature.point.undefine';

  /// 声明一个**自己的音源类型**（id 必须形如 `plugin.<自己的插件 id>.<类型名>`）
  ///
  /// 音源地址写成 `plugin.<自己的插件 id>.<类型名>://<自己的标识>`：主程序内置实现不解析这种
  /// 地址，播放时问钩子 `musicxx.player.source.resolve` 的处理器（注册处理器就是"提供解析"）。
  /// 参数 `id`（必填，与地址的 scheme 一致）、`title`（展示名）、`tag`（列表标签里的短名，
  /// 例如 `FTP`，主程序显示成 `[FTP]`；不写就用 `title`）、`depict`（一句话说明）、
  /// `srcDoc`（地址里写什么）。声明只用于自省与更清楚的报错：**解析能不能成看有没有注册解析
  /// 处理器**。同一 id 重复声明 = 覆盖（最后一次生效）；声明跟着插件生命周期走
  /// （禁用 / 卸载自动撤销），不持久化。
  static const String sourceDefine = 'musicxx.source.define';

  /// 收回自己声明的音源类型（参数 `id`）
  ///
  /// 一般不用调：插件被禁用 / 卸载时插件框架会撤销它的全部声明。
  static const String sourceUndefine = 'musicxx.source.undefine';

  /// 列出插件音源类型与音源解析链（自省）
  ///
  /// 返回 `{sources, resolveChain}`：`sources` = 各插件声明的音源类型，
  /// `resolveChain` = 音源解析链现在有谁在提供（与 `musicxx.feature.point.list`
  /// 看 `musicxx.player.source.resolve` 是同一份数据）。
  static const String sourceList = 'musicxx.source.list';

  /// 声明**这个音源类型可以浏览并挑歌**（目录列表、搜索、写操作都走这里）
  ///
  /// 与 `musicxx.source.define` 分开：`define` 是"这个类型能解析播放"，`browse` 是"这个类型
  /// 能浏览并挑歌"。一个类型可以只有前者（例如只做播放代理），也可以两者都有。
  ///
  /// 参数：`id`（必填，必须是自己命名空间下的音源类型 id）、`title`（菜单标题）、
  /// `root`（根目录，缺省 `/`）、`icon?`（插件目录里的 svg 路径）、`write?`（是否允许写）、
  /// `search?`（是否支持搜索）、`capabilities?`（覆盖默认能力名，键是 `list` /
  /// `resolveSongs` / `search` / `mkdir` / `rename` / `remove` / `info` / `open`）。
  /// 声明跟着插件生命周期走（禁用 / 卸载自动撤销），不持久化。
  ///
  /// 声明后插件自动出现在「添加歌曲」菜单里（一行，点开就是内置风格的挑歌弹窗），
  /// 数据由 `drive.list` 能力提供。
  static const String sourceBrowse = 'musicxx.source.browse';

  /// 读某个钩子当前的处理器列表（自省：谁注册了、什么形态、排第几）
  ///
  /// 参数：`hook`（钩子 id，如 `musicxx.media.spectrum.extract`）。
  /// 返回 `{hook, count, handlers, listAvailable}`；`listAvailable=false` 表示插件框架库太旧，
  /// 只能报处理器数量（此时看 `count`）。
  static const String hookHandlers = 'musicxx.hook.handlers';

  static const String hostOpenUrl = 'musicxx.host.openUrl';
  static const String hostShareText = 'musicxx.host.shareText';
  static const String hostClipboard = 'musicxx.host.clipboard';
  static const String hostGetPath = 'musicxx.host.getPath';

  static const String statsReportMemory = 'musicxx.stats.reportMemory';
  static const String statsReportMetric = 'musicxx.stats.reportMetric';
}
