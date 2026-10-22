// 自动生成（tools/gen_contract.dart ← tools/hooks.def.json）—— 请勿手改。
//
// 钩子 id 是跨边界稳定的约定：字符串值一旦发布不得修改，
// 只能新增或标记废弃。字段含义见 tools/hooks.def.json。
// ignore_for_file: type=lint, constant_identifier_names

/// 钩子模式（与 C++ `MUSICXX_PLUGIN_HOOK_MODE_*` 一致）
enum MusicxxPluginHookMode {
  /// 观察型：不等待、不裁决（入队即返回）
  observe(0),

  /// 裁决型：等待处理器链结果
  decision(1);

  const MusicxxPluginHookMode(this.code);

  final int code;
}

/// 多处理器裁决合并策略
enum MusicxxPluginDecisionPolicy {
  /// 首个非空裁决生效，后续处理器不再询问
  firstNonNull(0),

  /// 任一处理器 cancel/deny 即生效（最保守），patch 按优先级合并
  anyCancel(1),

  /// 所有 patch 依次深合并
  allMerge(2),

  /// 最后一个非空裁决生效
  lastWrite(3);

  const MusicxxPluginDecisionPolicy(this.code);

  final int code;
}

/// 派发方式
enum MusicxxPluginHookDispatch {
  /// 调用点就地等待裁决（`MusicxxPluginHooks.decide`；占用调用线程）
  sync(0),

  /// 不占用调用线程：观察型 = 入队即返回；裁决型 = 异步派发（`decideAsync`，
  /// 结果经 `musicxx.hook.decision.result` 事件回传）
  async(1);

  const MusicxxPluginHookDispatch(this.code);

  final int code;
}

/// 钩子 id 与元信息（由 `tools/hooks.def.json` 生成）
enum MusicxxPluginHookId {
  /// 载荷 `{version, platform, branch, installId, lang}`。主程序启动阶段（业务 Store 与插件框架都就绪之后）派发一次：适合做初始化自己的状态、登记定时器这类事，观察型没有裁决。
  appStart('musicxx.app.start', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, true, MusicxxPluginHookDispatch.async),
  /// 载荷 `{firstRun, restoredSong}`：`firstRun` = 是否首次启动，`restoredSong` = 恢复播放的歌曲（可能缺省）。首屏与数据都就绪后派发一次（比 `app.start` 晚），观察型。
  appReady('musicxx.app.ready', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, true, MusicxxPluginHookDispatch.async),
  appBackground('musicxx.app.background', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  appForeground('musicxx.app.foreground', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  appExit('musicxx.app.exit', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, false, MusicxxPluginHookDispatch.sync),
  appDeepLink('musicxx.app.deepLink', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, false, MusicxxPluginHookDispatch.sync),
  appUpgrade('musicxx.app.upgrade', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  /// 载荷 `{sid, song, mode, quality}`：`song` 是只读视图（`sid/name/artist/album/durationMs/year/genre/srcKey/audio[]/video[]`，不含音频直链），`mode` = 播放模式、`quality` = 当前音质。裁决只实现 `{"action":"skip"}` = 跳过本曲（走应用既有的切下一曲流程）；`patch`（换音源/改音质）当前版本未实现，会被记一条日志后忽略。异步派发：切歌不会被插件拖慢；等待期间若又来了新的切歌请求，本次裁决被丢弃（插件可以自己用 `sid` 判断）。
  playerBeforePlaySong('musicxx.player.beforePlaySong', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, true, MusicxxPluginHookDispatch.async),
  /// 载荷 `{sid, srcKey, src, index, tryLocalOrCache}`：`src` 是音源视图（`type/empty/srcKey/quality/durationMs/hashMd5/size`），`index` = 本轮解析的音源下标。裁决：`{"action":"skip"}` = 本轮不解析该音源（调用方接着试下一个）；`patch.src = {"type":"Local|UrlLink|Bili|...","src":"<新地址>","info":{...}}` = 只替换本轮使用的音源（不改动歌曲实体的音源列表），字段不合法会被忽略。异步派发（不卡住解析），等待期间切歌则本次裁决作废。
  playerSourceBeforeParse('musicxx.player.source.beforeParse', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, true, MusicxxPluginHookDispatch.async),
  /// 载荷 `{sid, srcKey, useSrcKey, parsedType, pathKind, hasHeaders, cacheStream}`：某个音源解析完成（拿到真正播放用的路径与请求头）之后派发。载荷里没有直链与 token，只报来源类型与路径种类（本地 / 缓存 / 远程流 / 带请求头）；观察型。
  playerSourceResolved('musicxx.player.source.resolved', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, true, MusicxxPluginHookDispatch.async),
  /// 载荷 `{srcKey, type, src, crudeType, crudeSrc, fileName, durationMs, quality, isMediaSrc, pluginType, caller}`：主程序要把一个音源解析成**播放器内核能打开的地址与请求头**时派发一次（外置 → 内置插件 → 应用自身，第一个给出合法结果的生效）。`src` = 本轮要解析的音源（插件自己的音源就是它写下的资源 id / 路径；内置来源是已经解析出来的临时直链或本地路径 —— 注册这条钩子的插件看得到这些内容，这是它能换地址的前提）；`crudeSrc` = 歌曲实体里保存的原始地址；`type` / `crudeType` = 对应的来源类型（`link` / `bili` / `aliDrive` / `local` 等；插件音源是空串）；`pluginType` = 本轮音源的插件音源类型 id（`plugin.<插件 id>.<类型名>`；不是插件音源则为空串）；`quality` = 用户当前音质偏好（枚举名小写）；`isMediaSrc` = false 表示这次要的是封面（媒体库来源）；`caller` = 谁在问（空串 = 主程序自己或命令行）。处理器返回 `{"src": {"path": "<地址>", "headers": {"Referer": "..."}, "kind": "url", "expireAtMs": 0, "size": 0, "durationMs": 0, "quality": "hq"}}`（`src` 也可以直接给一个地址字符串）：这就是播放器要打开的地址与请求头。`path` 支持网络地址、本地文件路径、`content://`、`asset://`；`kind` 取 `url` / `file` / `content` / `asset`，缺省按地址自己判断（判断错会让本地文件被当成网络流去做缓存）；`headers` 可省；`expireAtMs` 给临时地址的过期时间戳（毫秒，只用于排障）。**回答请用 `patch` 包一层**（写成 `patch.src`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历。给自己的音源给出地址与请求头、给内置来源换地址（加代理、换 CDN、统一请求头）都是这条路。不返回 / 结果不合法 = 这次不提供，主程序接着问内置实现。返回 `{"disable": true}` 是显式置空：这次不再问别的实现，该音源会解析失败（只在明确知道它不该播时用）。异步派发：主程序最多等 5 s（超时按没给处理，改走内置实现），处理器请尽快返回。主程序只在"该钩子有处理器"时才派发。
  playerSourceResolve('musicxx.player.source.resolve', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, true, MusicxxPluginHookDispatch.async),
  /// 未接入（已被 `musicxx.player.source.resolve` 取代）：换地址 / 提供播放地址的能力统一走 `musicxx.player.source.resolve` —— 那条钩子既解析插件自己的音源，也能覆盖内置解析，而且排在打开播放器之前的同一个位置。注册这条不会收到任何调用。
  playerBeforeOpen('musicxx.player.beforeOpen', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, false, MusicxxPluginHookDispatch.async),
  /// 载荷 `{state, sid, positionMs}`。`state` 是播放状态文本：播放 / 暂停 / 停止这三个事件给 `play`、`pause`、`stop`，其它状态变化给枚举名 `Play` / `Pause` / `Stop` / `Completed`；要判断当前是否在播放，请读状态镜像 `musicxx.state.env.isPlaying`（布尔）。观察型，只在播放状态变化时派发（播放进度不走这个钩子）。
  playerState('musicxx.player.state', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, true, MusicxxPluginHookDispatch.async),
  playerPosition('musicxx.player.position', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  playerSeek('musicxx.player.seek', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, false, MusicxxPluginHookDispatch.sync),
  playerVolume('musicxx.player.volume', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, false, MusicxxPluginHookDispatch.sync),
  playerSpeed('musicxx.player.speed', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, false, MusicxxPluginHookDispatch.sync),
  playerPitch('musicxx.player.pitch', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, false, MusicxxPluginHookDispatch.sync),
  /// 载荷 `{sid, srcKey, isNowUseSrc, errorCount, durationMs, positionMs}`（`errorCount` 已包含本次失败）。裁决：`{"action":"stop"}` = 停止播放；`{"action":"skip"}`（或 `cancel`）= 跳到下一曲；`{"action":"continue","patch":{"tryNextSrc":false}}` = 不再尝试当前源（跳过忽略错误并重试的分支，直接按主程序的换源/下一曲策略走）。异步派发（不占用调用线程，主程序不会被插件拖住）：等待期间切歌则本次裁决作废；处理器请尽快给出裁决，本次失败处理会等它结算。主程序只在"该钩子有处理器"时才派发。
  playerError('musicxx.player.error', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, true, MusicxxPluginHookDispatch.async),
  /// 载荷 `{sid, playedMs}`：一曲播放完成（正常结束或按完成处理）之后派发；观察型。它只表示这首歌放完了，不代表已经切歌（切歌另有 `musicxx.song.changed`）。
  playerCompleted('musicxx.player.completed', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, true, MusicxxPluginHookDispatch.async),
  playerQualityChanged('musicxx.player.qualityChanged', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  mediaNotification('musicxx.media.notification', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.sync),
  /// 载荷 `{sid, prevSid, song}`：`song` 与 `beforePlaySong` 的同一个只读视图，`prevSid` = 上一首的 sid（没有则为 null）。切歌时派发；派发前插件框架会先刷新状态镜像，所以处理器里同步读 `musicxx.state.song` 拿到的就是新歌。观察型。
  songChanged('musicxx.song.changed', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, true, MusicxxPluginHookDispatch.async),
  songInfoAnalyse('musicxx.song.info.analyse', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, false, MusicxxPluginHookDispatch.sync),
  songMetaWriteback('musicxx.song.meta.writeback', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  songBeforeAdd('musicxx.song.beforeAdd', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, false, MusicxxPluginHookDispatch.sync),
  songRemoved('musicxx.song.removed', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  playlistLoaded('musicxx.playlist.loaded', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  playlistFilter('musicxx.playlist.filter', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.allMerge, false, MusicxxPluginHookDispatch.async),
  songlistFilter('musicxx.songlist.filter', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.allMerge, false, MusicxxPluginHookDispatch.async),
  historyRecord('musicxx.history.record', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  localScanFile('musicxx.local.scan.file', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, false, MusicxxPluginHookDispatch.sync),
  localScanFinished('musicxx.local.scan.finished', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  lyricLoadBefore('musicxx.lyric.load.before', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, false, MusicxxPluginHookDispatch.async),
  lyricLoaded('musicxx.lyric.loaded', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  lyricTransform('musicxx.lyric.transform', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.allMerge, false, MusicxxPluginHookDispatch.async),
  lyricCurrent('musicxx.lyric.current', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  lyricSearchStr('musicxx.lyric.searchStr', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, false, MusicxxPluginHookDispatch.sync),
  lyricProvider('musicxx.lyric.provider', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, false, MusicxxPluginHookDispatch.async),
  iconRequest('musicxx.icon.request', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, false, MusicxxPluginHookDispatch.async),
  iconGenerated('musicxx.icon.generated', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  mediaInfoRequest('musicxx.media.info.request', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, false, MusicxxPluginHookDispatch.async),
  /// 已废弃（应用不派发）：应用从来没有派发过这个钩子，注册它不会收到任何事件，将来也不会收到。原先它想当【频谱数据算好了】的通知，现在改由两条现成的路承担：要高频读当前频谱用状态镜像 `musicxx.state.spectrum`（按需推送、每帧都能读到最新值）；要主动拿整曲结果用动作 `musicxx.host.request` 调能力 `GetAudioSpectrum`（或 JS 便捷封装 `musicxx.feature.call("GetAudioSpectrum")`）。
  mediaWaveReady('musicxx.media.wave.ready', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  /// 已废弃（应用不派发）：应用从来没有派发过这个钩子，注册它不会收到任何事件，将来也不会收到。原先它想当【副歌分析完成】的通知，现在改由两条现成的路承担：要主动拿结果用动作 `musicxx.host.request` 调能力 `AnalyseAudioChorus`（或 JS 便捷封装 `musicxx.feature.call("AnalyseAudioChorus")`，副歌区段的结果很小，适合按需查）；要读当前歌曲状态用状态镜像 `musicxx.state.song`。
  mediaChorusAnalysed('musicxx.media.chorus.analysed', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  /// 载荷 `{srcKey, name, artist, kind, path, durationMs, frames, binCount, frameHz, maxSeconds, caller}`（`caller` = 谁在问：空串 = 主程序自己取值或命令行，非空 = 主动调用它的插件 id）：主程序要提取一首歌的**整曲频谱**时派发一次（`path` 是本地/缓存文件路径，不是网络直链；`kind` = `local` / `cache` / `content` / `other`；`frames` 按时长推算、只作提示）。处理器返回 `{"spectrum": {"spectrums": "<base64>", "waveform": "<base64，可省>", "binCount": 256, "frameHz": 10}}` 就用插件的数据替代内置提取（内置实现是主程序自己用 mediaxx 提取）：`spectrums` 是整曲频点（帧数 × 256 个字节，0~255 表示 -80~0 dB，默认每秒 10 帧），`waveform` 是每帧 1 字节 RMS 响度（0~255 表示 -60~+20 dB；省掉或长度对不上时按每帧频点均值补一份近似响度）。不返回 / 没有 `spectrum` / 数据不合法（不是 base64、长度不是 256 的整数倍、`binCount` 与 `frameHz` 不是 256 与 10）= 这次不提供，主程序接着用已登记的实现（内置『音乐动效』）。**回答请用 `patch` 包一层**（写成 `patch.spectrum`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，多个实现时裸字段会被后面的处理器覆盖（只有一个实现时两种写法等价）。异步派发：主程序最多等 30 s（超时按没给处理）；等待期间切歌则本次结果会被丢弃（可用 `srcKey` 自行校验）。主程序只在"该钩子有处理器"时才派发。返回 `{"disable": true}` 是显式的**置空裁决**：主程序这次不再询问其它实现（内置『音乐动效』也不跑，播放页频谱与副歌分析都拿不到数据），这次结果不写缓存；置空只在本次询问有效，处理器被摘除（插件停用 / 卸载 / 关闭『拟声++』）后自动还原。要用它请先确认自己的数据已经算好（例如自己起线程提前算完），否则用户会看到功能凭空消失。**这个功能点对外开放调用**（动作 `musicxx.feature.point.call`）：外置插件、命令行与 agent 能主动调它并拿到原始数据，参数与结果见 `featurePoint.list` 里的 `callDoc`。调用同样按这条处理器链问一遍，但**不写主程序的值仓、不改主程序状态**（只读值仓，绝不记录置空标记），同一个功能点 + 同一个调用方的上一次调用没结束时拿到 `busy`。
  mediaSpectrumExtract('musicxx.media.spectrum.extract', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, true, MusicxxPluginHookDispatch.async),
  /// 载荷 `{srcKey, name, artist, kind, path, durationMs, hasWave, frames, binCount, frameHz, spectrums?, waveform?, config, caller}`（`caller` = 谁在问：空串 = 主程序自己取值或命令行，非空 = 主动调用它的插件 id）：主程序要分析一首歌的**副歌（高潮）区段**时派发一次。`hasWave` 为真时附带主程序已经拿到的整曲频谱（`spectrums` / `waveform` 的 base64，4 分钟歌曲约 800 KB —— 用不上的插件请不要注册这个钩子）；`config` 是内置算法的参数（`bandCount` / `hopFrames` / `windowFrames` / `minRepeatLagSeconds` / `segmentSimilarity` / `minSegmentSeconds` / `maxSegmentSeconds` / `mergeGapSeconds`）。处理器返回 `{"chorus": {"sections": [{"start": 64000, "end": 100000, "confidence": 0.8, "energy": 0.7, "similarity": 0.9, "repeat": 3}], "totalDuration": 253000, "analyseTimeMs": 120}}`（时间单位毫秒）就用插件的区段替代内置分析；`sections` 是必需项（空数组 = 分析过了、这首歌没有副歌），每段至少要有 `start` 与 `end`，其余字段缺失按 0 处理。不返回 / 缺少 `sections` / 一个区段都解析不出来 = 这次不提供。**回答请用 `patch` 包一层**（写成 `patch.chorus`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，多个实现时裸字段会被后面的处理器覆盖（只有一个实现时两种写法等价）。异步派发：主程序最多等 20 s；等待期间切歌则本次结果会被丢弃（可用 `srcKey` 自行校验）。主程序只在"该钩子有处理器"时才派发。返回 `{"disable": true}` 可以显式置空这个功能：主程序这次不再走内置分析（进度条上的副歌标记就没有了），处理器被摘除后自动还原。**这个功能点对外开放调用**（动作 `musicxx.feature.point.call`）：外置插件、命令行与 agent 能主动调它（参数与结果见 `featurePoint.list` 里的 `callDoc`）；调用会先取一次频谱，**这一层同样不写值仓**，也不改主程序状态，同一个功能点 + 同一个调用方的上一次调用没结束时拿到 `busy`。
  mediaChorusAnalyse('musicxx.media.chorus.analyse', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, true, MusicxxPluginHookDispatch.async),
  /// 载荷 `{srcKey, name, artist, hasCover, night, caller, cover}`：主程序要为一张封面取色（内置实现是封面颜色分析）时派发一次。`caller` = 谁在问（空串 = 主程序自己取值或命令行，非空 = 主动调用它的插件 id）；`cover` = 这次算哪一张图：`{kind: current | file | url | data, path?, url?, bytes?, imageBase64?, coverKey}`（`current` = 当前歌曲的封面，`file` / `url` / `data` 是调用方指定的那一张；`path` / `url` 是对应的地址，`data` 给 `bytes` 与 `imageBase64` —— 图太大时只有 `bytes`；`coverKey` 是这次的身份，插件可以按它自己缓存）。老插件不认 `cover` 也能跑：它们只看 `srcKey` / `name` / `artist` / `night`，对 `current` 这一种用法的结果是对的；别的来源它们给不出（返回空），主程序接着走自己的分析 —— 不会给错数据。处理器返回 `{"colors": {"main": "#rrggbb", ...}}`（键可取 `main` / `light` / `lightMuted` / `dark` / `darkMuted`，写法 `#rrggbb` 或 `#aarrggbb`）就用插件提供的这套颜色替代内置分析：它写进与内置分析同一批字段（**只在主程序自己问当前封面时**；外置通过 `musicxx.feature.point.call` 调它时只拿数据、不写字段），内置背景、插件背景与 `musicxx.icon.*` 来源都会拿到；不返回 / 没有 `colors` / 颜色一个都解析不出来 = 这次不提供，主程序照旧走内置分析。**回答请用 `patch` 包一层**（写成 `patch.colors`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，多个实现时裸字段会被后面的处理器覆盖（只有一个实现时两种写法等价）。异步派发：不会卡住渲染准备；等待期间切歌时本次结果会被丢弃（可用 `srcKey` 自行校验）。返回 `{"disable": true}` 可以显式关掉封面取色：主程序这次不再走内置取色（保持上一次的颜色），处理器被摘除后自动还原。**这个功能点对外开放调用**（动作 `musicxx.feature.point.call`）：外置插件、命令行与 agent 能主动调它并拿到 5 个原始色槽位，参数与结果见 `featurePoint.list` 里的 `callDoc`。**调用没有副作用**：不写封面取色字段、不改内置背景（要改观感请用 `musicxx.media.palette.set`），同一个功能点 + 同一个调用方的上一次调用没结束时拿到 `busy`。
  mediaPaletteProvide('musicxx.media.palette.provide', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, true, MusicxxPluginHookDispatch.async),
  /// 载荷 `{srcKey, name, artist, hasCover, kind}`（`kind` = `local` / `cache` / `content` / `asset` / `network`，没有封面时为空串）：当前歌曲的封面发生变化（换歌、同一首歌换了封面图、封面被清空）时派发一次。**载荷里没有直链、也没有图片字节** —— 要图片自己调 `musicxx.media.cover`（可配 `bind` 绑成纹理），要在着色器里采样就声明渲染项的 `cover` / `image`。观察型、异步：不卡住播放与封面加载；同一个封面源被重复设置不会重复派发。用 `musicxx.hooks.unregister("musicxx.media.cover.changed")` 移除后就不再收到（插件停用 / 卸载、关闭『拟声++』时插件框架也会自动摘掉处理器）。
  mediaCoverChanged('musicxx.media.cover.changed', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, true, MusicxxPluginHookDispatch.async),
  netRequestBefore('musicxx.net.request.before', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, false, MusicxxPluginHookDispatch.async),
  netResponseAfter('musicxx.net.response.after', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  netServerRoute('musicxx.net.server.route', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, false, MusicxxPluginHookDispatch.async),
  netMcpTools('musicxx.net.mcp.tools', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.allMerge, false, MusicxxPluginHookDispatch.sync),
  netLanEvent('musicxx.net.lan.event', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  downloadBefore('musicxx.download.before', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, false, MusicxxPluginHookDispatch.async),
  downloadCompleted('musicxx.download.completed', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  cacheBeforeTrim('musicxx.cache.beforeTrim', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, false, MusicxxPluginHookDispatch.sync),
  cachePathRequest('musicxx.cache.pathRequest', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, false, MusicxxPluginHookDispatch.sync),
  uiHomeEntries('musicxx.ui.home.entries', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.allMerge, false, MusicxxPluginHookDispatch.sync),
  uiSongActions('musicxx.ui.song.actions', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.allMerge, false, MusicxxPluginHookDispatch.sync),
  uiPlaylistActions('musicxx.ui.playlist.actions', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.allMerge, false, MusicxxPluginHookDispatch.sync),
  uiRouteResolve('musicxx.ui.route.resolve', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, false, MusicxxPluginHookDispatch.sync),
  uiPageEnter('musicxx.ui.page.enter', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  uiPageLeave('musicxx.ui.page.leave', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  uiThemeChanged('musicxx.ui.theme.changed', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  uiNotify('musicxx.ui.notify', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.anyCancel, false, MusicxxPluginHookDispatch.sync),
  uiUserAction('musicxx.ui.user.action', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  scriptBound('musicxx.script.bound', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  scriptDisposed('musicxx.script.disposed', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  scriptActionExecuted('musicxx.script.action.executed', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  clockingTick('musicxx.clocking.tick', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  listenTogetherEvent('musicxx.listenTogether.event', MusicxxPluginHookMode.observe, MusicxxPluginDecisionPolicy.lastWrite, false, MusicxxPluginHookDispatch.async),
  /// 载荷 `{point, args, caller}`：有人要算一个**外置插件声明的功能点**时派发一次（`point` = 功能点 id，`args` = 调用方给的参数，`caller` = 调用方插件 id，空串 = 应用自己或命令行）。这是**一条通用的实现通道**：插件声明的功能点共用它，而不是每个点各占一个钩子 id（插件框架只认固定的钩子表，见 docs/plugin-guide.md）。处理器只实现自己负责的点：不是自己的点直接返回空（空 = 不回答）；是自己的点就返回 `{"patch": {"value": <任意 JSON>}}` —— **必须用 `patch` 包一层**，插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，否则后面的处理器会覆盖你的结果。`value` 键就是“我答了”的判据（值可以是 null，例如“算过了、结果是空”）。返回 `{"disable": true}` 是显式置空：这次不再问别的实现。**同一条通道上所有实现都会收到同一次调用**，请按 `point` 过滤，不要假设只有自己收到。不要回答自己发起的调用（`caller` 等于自己的插件 id 时返回空；SDK 糖会自动做）。主程序不设超时，最多等到调用方自己的动作超时；同一个功能点 + 同一个调用方的上一次调用没结束时，调用方拿到 `busy`。功能点由插件用动作 `musicxx.feature.point.define` 声明、`musicxx.feature.point.undefine` 收回，声明跟着插件生命周期走。
  featurePointCall('musicxx.feature.point.call', MusicxxPluginHookMode.decision, MusicxxPluginDecisionPolicy.firstNonNull, true, MusicxxPluginHookDispatch.async),
  ;

  const MusicxxPluginHookId(
    this.id,
    this.mode,
    this.policy,
    this.wired,
    this.dispatch,
  );

  /// 跨边界稳定 id（`musicxx.<域>.<名>`）
  final String id;

  /// 观察型 / 裁决型
  final MusicxxPluginHookMode mode;

  /// 裁决合并策略（观察型无意义）
  final MusicxxPluginDecisionPolicy policy;

  /// 主程序是否已经接入这个钩子：true = 当前版本会派发；
  /// false = 约定已固定，但主程序还没有接入（注册不会报错，当前版本也不会触发）
  final bool wired;

  /// 派发方式（是否占用调用线程；见 `tools/hooks.def.json`）
  final MusicxxPluginHookDispatch dispatch;

  /// 是否裁决型（观察型不参与合并）
  bool get isDecision => mode == MusicxxPluginHookMode.decision;

  /// 是否为异步派发（观察型恒为 true；裁决型表示调用点用 `decideAsync`）
  bool get isAsyncDispatch => dispatch == MusicxxPluginHookDispatch.async;

  /// 按 id 反查（未知 id 返回 null；插件注册未知钩子会被插件框架拒绝）
  static MusicxxPluginHookId? tryFromId(String id) => _byId[id];

  static final Map<String, MusicxxPluginHookId> _byId = {
    for (final MusicxxPluginHookId hook in values) hook.id: hook,
  };
}

