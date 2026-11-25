// 槽位 / 变量清单生成物的自检
//
// 校验的是"生成物和定义文件一致"这一层（`gen_contract.dart` 的 `--check` 已经做过内容比对），
// 以及清单本身的规则：id 前缀、kind 合法、C 宏唯一、变量只允许 `musicxx.` / `scope:musicxx.`
// 两种前缀、作用域变量挂在自己的槽位上。
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/src/ui_slots.g.dart';
import 'package:musicxx_extern_plugin/src/ui_variables.g.dart';

List<Map<String, Object?>> _readDef(String name) {
  final File file = File('tools/$name');
  final Object? value = jsonDecode(file.readAsStringSync());
  return <Map<String, Object?>>[
    for (final Object? item in value as List)
      if (item is Map<String, Object?>) item,
  ];
}

void main() {
  group('槽位清单', () {
    test('定义文件与生成物条数一致', () {
      final List<Map<String, Object?>> def = _readDef('ui_slots.def.json');
      expect(kMusicxxUiSlots.length, def.length);
      for (int i = 0; i < def.length; i++) {
        expect(kMusicxxUiSlots[i].id, def[i]['id']);
        expect(kMusicxxUiSlots[i].kind.name, def[i]['kind']);
      }
    });

    test('id 前缀、简称唯一', () {
      final Set<String> full = <String>{};
      final Set<String> short = <String>{};
      for (final MusicxxUiSlotSpec_c slot in kMusicxxUiSlots) {
        expect(
          slot.id.startsWith(kMusicxxUiSlotPrefix),
          isTrue,
          reason: slot.id,
        );
        expect(slot.label.isNotEmpty, isTrue, reason: slot.id);
        expect(full.add(slot.id), isTrue, reason: '重复的槽位 id: ${slot.id}');
        expect(
          short.add(slot.shortName),
          isTrue,
          reason: '重复的槽位简称: ${slot.shortName}',
        );
      }
    });

    test('要出现在用户列表里的项都在 inList 里，插入 / 替换位置不在', () {
      for (final MusicxxUiSlotSpec_c slot in kMusicxxUiSlots) {
        switch (slot.kind) {
          case MusicxxUiSlotKind.item:
          case MusicxxUiSlotKind.selectable:
          case MusicxxUiSlotKind.pageBackground:
            expect(slot.inList, isTrue, reason: slot.id);
          case MusicxxUiSlotKind.insert:
          case MusicxxUiSlotKind.replace:
          case MusicxxUiSlotKind.searchType:
          case MusicxxUiSlotKind.page:
          case MusicxxUiSlotKind.overlay:
            break;
        }
      }
    });

    test('可选样式与页面背景的槽位同时是声明项所在的位置', () {
      // 这三类样式靠"声明项 + 同一槽位上的内容"配合：候选列表读声明项，画面读内容
      for (final MusicxxUiSlotSpec_c slot in kMusicxxUiSlots) {
        if (slot.kind == MusicxxUiSlotKind.selectable ||
            slot.kind == MusicxxUiSlotKind.pageBackground) {
          expect(slot.inList, isTrue, reason: slot.id);
        }
      }
    });

    test('简称能补成全名，查询能拿到同一份元信息', () {
      for (final MusicxxUiSlotSpec_c slot in kMusicxxUiSlots) {
        expect(musicxxUiSlotFullName(slot.shortName), slot.id);
        expect(musicxxUiSlotFullName(slot.id), slot.id);
        expect(musicxxUiSlotOf(slot.shortName)?.id, slot.id);
        expect(musicxxUiSlotOf(slot.id)?.id, slot.id);
      }
      expect(
        musicxxUiSlotFullName('plugin.foo.ui.toolbar'),
        'plugin.foo.ui.toolbar',
      );
      expect(musicxxUiSlotOf('plugin.foo.ui.toolbar'), isNull);
    });

    test('C 头里的宏与 Dart 常量指向同一批 id', () {
      final String header = File(
        'src/sdk/include/musicxx/plugin/api/ui_slots.g.h',
      ).readAsStringSync();
      final RegExp macro = RegExp(
        r'#define (MUSICXX_PLUGIN_UI_SLOT_[A-Z0-9_]+) "([^"]+)"',
      );
      final Map<String, String> byId = <String, String>{};
      for (final RegExpMatch match in macro.allMatches(header)) {
        byId[match.group(2)!] = match.group(1)!;
      }
      expect(byId.length, kMusicxxUiSlots.length);
      final Set<String> macroNames = <String>{};
      for (final MusicxxUiSlotSpec_c slot in kMusicxxUiSlots) {
        expect(byId.containsKey(slot.id), isTrue, reason: slot.id);
        expect(
          macroNames.add(byId[slot.id]!),
          isTrue,
          reason: '重复的宏名: ${byId[slot.id]}',
        );
      }
    });
  });

  group('变量清单', () {
    test('定义文件与生成物条数一致', () {
      final List<Map<String, Object?>> def = _readDef('ui_variables.def.json');
      expect(kMusicxxUiVariables.length, def.length);
      for (int i = 0; i < def.length; i++) {
        expect(kMusicxxUiVariables[i].id, def[i]['id']);
      }
    });

    test('前缀只有 musicxx. 与 scope:musicxx.，id 唯一', () {
      final Set<String> ids = <String>{};
      for (final MusicxxUiVariableSpec_c item in kMusicxxUiVariables) {
        final String rest = item.isScope
            ? item.id.substring(kMusicxxUiVariableScopePrefix.length)
            : item.id;
        expect(rest.startsWith('musicxx.'), isTrue, reason: item.id);
        expect(item.depict.isNotEmpty, isTrue, reason: item.id);
        expect(ids.add(item.id), isTrue, reason: '重复的变量: ${item.id}');
      }
    });

    test('作用域变量挂在已有的槽位上，全局变量任何槽位都可用', () {
      for (final MusicxxUiVariableSpec_c item in kMusicxxUiVariables) {
        for (final String slot in item.slots) {
          expect(
            kMusicxxUiSlotIndex.containsKey(slot),
            isTrue,
            reason: '${item.id} 挂到了不存在的槽位 $slot',
          );
        }
        if (!item.isScope) {
          expect(item.slots, isEmpty, reason: '全局变量不该限定槽位: ${item.id}');
          expect(
            musicxxUiVariableAllowedOn(
              item.id,
              MusicxxUiSlots_c.playingLyricTop,
            ),
            isTrue,
          );
        }
      }
    });

    test('槽位可用性判定', () {
      const String songlistName = 'scope:musicxx.songlist.name';
      expect(
        musicxxUiVariableAllowedOn(
          songlistName,
          MusicxxUiSlots_c.pageBackground,
        ),
        isTrue,
      );
      expect(
        musicxxUiVariableAllowedOn(
          songlistName,
          MusicxxUiSlots_c.playingLyricTop,
        ),
        isFalse,
      );
      // 嵌套槽位继承外层作用域
      expect(
        musicxxUiVariableAllowedOn(
          songlistName,
          MusicxxUiSlots_c.playingLyricTop,
          nested: true,
        ),
        isTrue,
      );
      // 没声明过的变量（含插件自定义）不属于这份表
      expect(
        musicxxUiVariableAllowedOn('plugin.foo.bar', MusicxxUiSlots_c.page),
        isFalse,
      );
    });

    test('C 头的变量表与 Dart 表一致', () {
      final String header = File(
        'src/sdk/include/musicxx/plugin/api/ui_variables.g.h',
      ).readAsStringSync();
      final RegExp row = RegExp(r'\{"([^"]+)", (true|false)\},');
      final Map<String, bool> byId = <String, bool>{};
      for (final RegExpMatch match in row.allMatches(header)) {
        byId[match.group(1)!] = match.group(2) == 'true';
      }
      expect(byId.length, kMusicxxUiVariables.length);
      for (final MusicxxUiVariableSpec_c item in kMusicxxUiVariables) {
        expect(byId[item.id], item.isScope, reason: item.id);
      }
    });
  });
}
