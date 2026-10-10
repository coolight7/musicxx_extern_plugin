# Android 平台接入（插件框架库随 APK 分发）

> **术语约定**：本文档中的**主程序**指的就是 **musicxx**（Flutter/Dart 业务层）；外置插件的 C++ 框架层称为**插件框架**；插件目前分为**动态库插件**与**JS插件**。

本目录是 `musicxx_extern_plugin` 的 Android 平台工程（Flutter FFI 插件）。
它的职责有两个：**按需交叉编译插件框架库**，以及**把插件框架库放进 APK**，让主程序能被 Dart 侧加载。

## 为什么必须打进 APK

Android 7（API 24）起，动态链接器只允许应用从 APK 的原生库目录（以及系统目录）加载动态库。
放在应用数据目录里的 `.so` 会失败：

```text
dlopen failed: library "/data/user/0/<包名>/files/xxx.so" is not accessible for the namespace "classloader-namespace"
```

所以：

- **插件框架库**（`libmusicxx_extern_plugin.so`）必须随 APK 分发，本模块负责收集；
- **用户安装的动态库插件**（放在 `<应用支持目录>/musicxx/extern_plugin/plugins/<插件id>/`）在部分
  设备/系统版本上会因同一条限制装载失败 —— 插件框架按"安全降级"处理：该插件标记为不可用、给出原因、
  不重试也不影响其它插件。JS插件不受影响。

## 构建插件框架库

**默认会自动构建**：`flutter build apk` / `flutter run` 时，`android/build.gradle` 发现本次要用的 ABI
缺少插件框架库就调包内构建脚本的自动模式（已是最新则秒退，每个 ABI 各构建一次）。关掉自动构建
（`-PmusicxxExternPluginAutoBuild=false` 或环境变量 `MUSICXX_EXTERN_PLUGIN_AUTO_BUILD=0`）后，按下手工构建：

```bash
# Windows（NDK 位置自动解析：-AndroidNdk > $env:ANDROID_NDK_HOME > $env:ANDROID_HOME/ndk 下版本号最大的一个）
pwsh -NoProfile -File tools/build_native.ps1 -Android -Abi arm64-v8a
pwsh -NoProfile -File tools/build_native.ps1 -Android -Abi arm64-v8a -Config Debug
pwsh -NoProfile -File tools/build_native.ps1 -Android -Abi armeabi-v7a
pwsh -NoProfile -File tools/build_native.ps1 -Android -Abi x86_64

# Linux / macOS（脚本的 Android 分支与 Windows 侧同一套参数）
ANDROID_NDK_HOME=<ndk> tools/build_native.sh --android --abi arm64-v8a
```

Release 用于发布的 APK、Debug 用于调试构建；自动构建默认只产出 Release（Debug 原生产物是另一整条依赖链），
要 Debug 就用 `-PmusicxxExternPluginConfig=Debug` 或上面的 `-Config Debug` 手工构建一次。

产物：

```text
<包>/.native/output/android-<abi>-<配置>/bin/libmusicxx_extern_plugin.so
<包>/.native/build/android-<abi>-<配置>/musicxx-extern-plugin-install/bin/libmusicxx_extern_plugin.so   # 未剥离符号，留作崩溃符号化
```

## 库是怎么进 APK 的

`android/build.gradle` 会：

1. 本次要用的 ABI（Flutter 的 `target-platform`）里缺库时，先调包内构建脚本的自动模式补上；
2. 扫描上面两个目录（以及仓库预置目录 `resource/libs/android/<abi>/`），按 ABI 找到插件框架库；
3. 把它们复制到 `<模块构建目录>/musicxxExternPluginJniLibs/<build type>/<abi>/`，并把这个目录注册成
   对应 build type 的 `jniLibs` 源目录；
4. 上述两个任务都挂在该 build type 的 `preBuild` 之前，因此 AGP 合并、打包时库已经在位
   （收集任务在**执行期**重新扫描一遍，所以"本次刚自动构建出来的库"也会被收进这个 APK，不会晚一版）。

Debug 与 Release 各自收集自己那一份（不会把 Debug 库打进 Release 包）。某个 ABI 没构建时只警告、
不中断构建：APK 里就没有那个 ABI 的库，对应设备上外部插件不可用（应用与其它功能不受影响）。

显式指定库目录（CI 用；布局是 `<abi>/libmusicxx_extern_plugin.so`）：

```bash
flutter build apk --release -PmusicxxExternPluginLibDir=/path/to/libs
# 或环境变量 MUSICXX_EXTERN_PLUGIN_ANDROID_LIB_DIR=/path/to/libs
```

只收集**本次构建真正要用的 ABI**：Flutter 传下来的 `-Ptarget-platform`（如
`android-arm64`）会映射成 ABI，只把这些 ABI 的库放进 APK；没有该属性（直接用 Gradle 构建）时
收集全部已构建的 ABI。每个 ABI 的插件框架库约 3 MB，单 ABI 调试时可以用这条来缩小包体。

## 验证

```bash
flutter build apk --debug --target-platform android-arm64
unzip -l build/app/outputs/flutter-apk/app-debug.apk | grep libmusicxx_extern_plugin
# 或者装到设备上后：
adb shell run-as <包名> ls -l /data/app/*/lib/<abi>/libmusicxx_extern_plugin.so
```

Dart 侧加载顺序（`lib/src/native_library.dart`）：环境变量 `MUSICXX_EXTERN_PLUGIN_LIBRARY` →
库名（`libmusicxx_extern_plugin.so`，由系统解析到 APK 的 `lib/<abi>/`）。桌面端的 `.native/` 候选在
Android 上不参与（Android 没有可执行文件旁的库目录）。

## 已知限制

- `-RunTests` 在 Android 上跳过：测试可执行文件是给设备编的，主程序所在系统/本机上跑不起来。需要在设备上跑时，
  把 `<安装前缀>/bin/` 与 `<安装前缀>/plugins/` 推到设备（例如 `/data/local/tmp/`），
  用 `LD_LIBRARY_PATH=<bin> ./musicxx_extern_plugin_test <plugins>` 执行。
- `armeabi-v7a`（32 位 ARM）未实测：依赖链里的 simdjson 等库需要自行确认 32 位 ARM 可编译。
- 用户安装的动态库插件受系统的动态库加载限制，可能装载失败（见上）。
