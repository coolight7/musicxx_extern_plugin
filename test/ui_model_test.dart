/// 声明式 UI 扩展模型单测（纯 Dart，不加载原生库）
///
/// 覆盖三种动作形态（与插件页面里的动作同一套写法）、命名空间校验后的 id 形态、
/// 快照解析容错与整批替换。
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';

void main() {
  test('解析插件框架快照：字段读取、排序与动作类型', () {
    final List<MusicxxPluginUIItem> items = MusicxxPluginUIItem.parseList(<Object?>[
      <String, Object?>{
        'id': 'plugin.demo.songInfo',
        'plugin': 'demo',
        'type': MusicxxPluginUIType.songAction,
        'order': 900,
        'data': <String, Object?>{
          'title': '菜单项',
          'action': <String, Object?>{'kind': 'dispatch', 'name': 'probe', 'args': <String, Object?>{'a': 1}},
        },
      },
      <String, Object?>{
        'id': 'plugin.demo.card',
        'plugin': 'demo',
        'type': MusicxxPluginUIType.homeEntry,
        'order': 100,
        'data': <String, Object?>{
          'title': '入口',
          'subtitle': '说明',
          // 图标是插件自己带的 svg：写相对插件目录的路径（resource/svg/ 只是推荐位置）
          'icon': 'resource/svg/entry.svg',
          'action': <String, Object?>{'kind': 'route', 'route': 'ext://demo/detail'},
        },
      },
    ]);

    expect(items.length, 2);
    // 排序: home.entry 在 song.action 之前 (类型字典序与插件框架快照一致)
    expect(items.first.type, MusicxxPluginUIType.homeEntry);

    final MusicxxPluginUIItem card = items.first;
    expect(card.name, 'card');
    expect(card.title, '入口');
    expect(card.subtitle, '说明');
    expect(card.icon, 'resource/svg/entry.svg');
    expect(card.actionKind, MusicxxPluginUIActionKind.route);
    expect(card.routePlugin, 'demo');
    expect(card.viewId, 'detail');

    final MusicxxPluginUIItem songItem = items.last;
    expect(songItem.actionKind, MusicxxPluginUIActionKind.dispatch);
    expect(songItem.capabilityName, 'probe');
    expect(songItem.actionArgs['a'], 1);
  });

  test('动作写法与界面描述层一致：短写/command/未知写法', () {
    MusicxxPluginUIItem itemOf(Object? action) =>
        MusicxxPluginUIItem.fromJson(<String, Object?>{
          'id': 'plugin.demo.songInfo',
          'plugin': 'demo',
          'type': MusicxxPluginUIType.songAction,
          'data': <String, Object?>{'title': '菜单项', 'action': action},
        });

    // 字符串短写 = 调用本插件能力
    final MusicxxPluginUIItem shortForm = itemOf('probe');
    expect(shortForm.actionKind, MusicxxPluginUIActionKind.dispatch);
    expect(shortForm.capabilityName, 'probe');

    // 官方动作用 command；命令名只在 command 形态下给出
    final MusicxxPluginUIItem command = itemOf(<String, Object?>{
      'kind': 'command',
      'name': 'musicxx.ui.notify',
      'args': <String, Object?>{'text': 'hi'},
    });
    expect(command.actionKind, MusicxxPluginUIActionKind.command);
    expect(command.hostActionName, 'musicxx.ui.notify');
    expect(command.capabilityName, isNull);

    // 旧写法（capability / action）不再解析：当成没有动作
    expect(itemOf(<String, Object?>{'kind': 'capability', 'name': 'probe'}).action, isNull);
    expect(itemOf(<String, Object?>{'kind': 'action', 'name': 'musicxx.ui.notify'}).action, isNull);
    expect(itemOf(<String, Object?>{'kind': 'unknown'}).action, isNull);
  });

  test('route 动作允许指向任意插件：拆出目标插件与视图 id，非法地址为空', () {
    MusicxxPluginUIItem itemOf(String route) =>
        MusicxxPluginUIItem.fromJson(<String, Object?>{
          'id': 'plugin.demo.entry',
          'plugin': 'demo',
          'type': MusicxxPluginUIType.homeEntry,
          'data': <String, Object?>{
            'title': '入口',
            'action': <String, Object?>{'kind': 'route', 'route': route},
          },
        });

    // 本插件的页面
    final MusicxxPluginUIItem own = itemOf('ext://demo/card');
    expect(own.routePlugin, 'demo');
    expect(own.viewId, 'card');

    // 别的插件的页面（页面由对方插件自己绘制，发起方拿不到对方的数据）
    final MusicxxPluginUIItem other = itemOf('ext://other_plugin/settings');
    expect(other.routePlugin, 'other_plugin');
    expect(other.viewId, 'settings');

    // 视图 id 里带斜杠（框架只要求 ext:// 之后有 id 与视图，其余原样当视图 id）
    final MusicxxPluginUIItem nested = itemOf('ext://other_plugin/a/b');
    expect(nested.routePlugin, 'other_plugin');
    expect(nested.viewId, 'a/b');

    // 非法地址：缺插件 id / 缺视图 id / 不是 ext:// → 都不是可用的 route 目标
    for (final String bad in <String>[
      'ext://card',
      'ext://demo/',
      'ext:///card',
      'musicxx:settings',
      '',
    ]) {
      final MusicxxPluginUIItem item = itemOf(bad);
      expect(item.routePlugin, isNull, reason: 'routePlugin: $bad');
      expect(item.viewId, isNull, reason: 'viewId: $bad');
    }

    // 非 route 动作没有页面目标
    final MusicxxPluginUIItem command = MusicxxPluginUIItem.fromJson(<String, Object?>{
      'id': 'plugin.demo.cmd',
      'plugin': 'demo',
      'type': MusicxxPluginUIType.songAction,
      'data': <String, Object?>{
        'title': '菜单项',
        'action': <String, Object?>{'kind': 'command', 'name': 'musicxx.ui.notify'},
      },
    });
    expect(command.routePlugin, isNull);
    expect(command.viewId, isNull);
  });

  test('容错：未知字段/非法项被忽略，缺省动作视为无动作', () {
    final List<MusicxxPluginUIItem> items = MusicxxPluginUIItem.parseList(<Object?>[
      <String, Object?>{'id': '', 'type': 'musicxx.ui.home.entry'}, // 缺 id → 跳过
      <String, Object?>{'id': 'plugin.demo.x'}, // 缺 type → 跳过
      'not-a-map', // 类型错 → 跳过
      <String, Object?>{
        'id': 'plugin.demo.plain',
        'plugin': 'demo',
        'type': MusicxxPluginUIType.playingBackground,
        'data': <String, Object?>{
          'title': '背景样式',
          'shader': <String, Object?>{'bundle': 'shader/bg.shaderbundle'},
          'action': <String, Object?>{'kind': 'none'},
        },
      },
    ]);

    expect(items.length, 1);
    expect(items.single.action, isNull);
    expect(items.single.actionKind, MusicxxPluginUIActionKind.none);
  });

  test('按插件整批替换（musicxx.ui.changed 的语义）', () {
    final MusicxxPluginUIItem a = MusicxxPluginUIItem.fromJson(<String, Object?>{
      'id': 'plugin.a.card',
      'plugin': 'a',
      'type': MusicxxPluginUIType.homeEntry,
      'data': <String, Object?>{'title': 'A'},
    });
    final MusicxxPluginUIItem b = MusicxxPluginUIItem.fromJson(<String, Object?>{
      'id': 'plugin.b.card',
      'plugin': 'b',
      'type': MusicxxPluginUIType.homeEntry,
      'data': <String, Object?>{'title': 'B'},
    });
    final MusicxxPluginUIItem a2 = MusicxxPluginUIItem.fromJson(<String, Object?>{
      'id': 'plugin.a.new',
      'plugin': 'a',
      'type': MusicxxPluginUIType.songAction,
      'data': <String, Object?>{'title': 'A2'},
    });

    final List<MusicxxPluginUIItem> replaced =
        MusicxxPluginUIItems.replacePlugin(<MusicxxPluginUIItem>[a, b], 'a', <MusicxxPluginUIItem>[a2]);
    expect(replaced.length, 2);
    expect(replaced.any((MusicxxPluginUIItem item) => item.id == 'plugin.a.card'), isFalse);
    expect(replaced.any((MusicxxPluginUIItem item) => item.id == 'plugin.b.card'), isTrue);
    expect(replaced.any((MusicxxPluginUIItem item) => item.id == 'plugin.a.new'), isTrue);

    expect(MusicxxPluginUIItems.byType(replaced, MusicxxPluginUIType.songAction).single.id, 'plugin.a.new');
  });
}
