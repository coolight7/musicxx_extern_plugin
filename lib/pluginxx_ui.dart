/// 插件界面描述层（`pluginxx_ui` 的转发出口）
///
/// 应用侧与插件侧只用这一个导入路径，不必直接依赖 `package:pluginxx_ui`：
/// ```dart
/// import 'package:musicxx_extern_plugin/pluginxx_ui.dart';
///
/// final UiDocument doc = parseDocument(jsonFromPlugin);
/// final UiDocument renderable = adaptDocument(doc, externPluginCapabilities());
/// ```
///
/// 定义与文档见包内子模块 `src/third_party/cxx_pluginxx_ui`（`schema/` 是唯一组件定义，
/// `docs/ui-schema.md` 是生成出来的组件表与适配规则）。
library;

export 'package:pluginxx_ui/pluginxx_ui.dart';
