# 实施记录

## 进度

- [x] 现状调查（基线构建失败点、新库 API 形状）
- [x] 方案文档
- [ ] 阶段 1：业务表 ABI 异步化（`plugin_api.h` + 版本号）
- [ ] 阶段 2：宿主表实现（`musicxx_host_tables.cpp` / `musicxx_host_ui.cpp` / `musicxx_host.h`）
- [ ] 阶段 3：宿主派发不占 IO 线程
- [ ] 阶段 4：原生 SDK kit 协程化
- [ ] 阶段 5：JS 引擎与 JS SDK 异步化
- [ ] 阶段 6：示例与原生测试
- [ ] 阶段 7：文档、版本号、CHANGELOG、生成物

## 基线构建结论（改动前）

`tools/build_native.ps1` 在依赖重建后失败，错误集中在：

- `musicxx_host_tables.cpp(1909..1911)`：广播表成员函数指针与新版 `PluginxxBroadcastsIface` 不匹配；
- `musicxx_host.h(144)`：`pluginDestroySymbol()` 不再覆写任何基类虚函数；
- `js_engine.cpp(2673..2677)`：内置插件槽位表用了旧的 `get_info/create/start/stop/destroy` 形状，
  `PluginxxBuiltinInfo` 已没有 `start` 字段；
- `js_engine.cpp(3545..3552 / 4454..4463)`：`subscribe_broadcast(_ex)` 少了新的完成回调参数。

## 细节记录

（按阶段补充：改了哪些文件、怎么验证、遇到的问题）
