# 全异步化实施记录

## 当前进度

方案已确认（2026-11-25），未开工。按阶段推进，每完成一步在此记"改了哪些文件 + 怎么验证"。

## 任务列表

- [ ] 阶段 0：基线对照用例（慢处理器占住框架）+ 记录现有测试数量
- [ ] 阶段 1：子模块 pluginxx 删 `sync`、能力表 v9、插件 ABI 10、`AsyncReply`
- [ ] 阶段 2：宿主派发改协程链并搬到插件线程；删 `run_sync`；原生内容生成搬插件线程；
      内部函数去 `Async` 后缀
- [ ] 阶段 3：JS 处理器统一异步完成
- [ ] 阶段 4：C ABI 7（全部操作提交化，含发布 / 订阅 / 日志 / 回复 / 关机；删 `plugin_load_sync`）
- [ ] 阶段 5：Dart 包请求总线 + 全 `Future` + 本地缓存 + 去掉 `_async` 命名
- [ ] 阶段 6：主程序接入（Store / 内容传输 / 广播取值 / 能力调用）
- [ ] 阶段 7：示例插件、原生测试、Dart 测试改造
- [ ] 阶段 8：文档与版本号（C ABI 7 / 插件 ABI 10 / 框架 `2026.1125.1`）
- [ ] 阶段 9：全量验证与基准

## 已确认的边界

1. 纯注册表读也走提交（`plugin_list` / `ui_*` / `stats` / `debug_info` / `capability_handlers`）。
2. 无需结果的操作同样回一条确认事件，可以 `await`；不关心就 `.ignore()`。
3. 删除全部同步函数，名称不再带 `_async` / `Async`；只留 `host_create` / `host_destroy`、
   事件通道与纯函数。
4. 版本号：C ABI 6 → 7、插件 ABI 9 → 10、能力表 v8 → v9、框架 `2026.1125.1`。
5. 删除 Dart 侧 `capabilities.on/off`。
6. `initDo` 只等 `host.start` 的结果，扫描与加载放后台。

两个实施取舍：`host_create` / `host_destroy` 保持同步（句柄）；子模块 `sleepAsync` /
`asyncOffload` 不改名（共用库公开名字）。

## 基线（开工前记录）

- 原生测试：`musicxx_extern_plugin_test` 518 checks / 0 failed；`musicxx_pluginxx_test` 283 checks / 0 failed。
- 契约生成物：`dart run tools/gen_contract.dart --check` 一致。
- 版本：框架 `2026.1124.1`、插件 ABI 9、C ABI 6、能力表 v8、JS 3/3。

## 每步记录

（实施时逐条补：改了哪些文件、验证命令与结果、与计划不一致的地方。）

## 未验证

（实施后补：只做了构建、没有实跑的平台与场景。）
