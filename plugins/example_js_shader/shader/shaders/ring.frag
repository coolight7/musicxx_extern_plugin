#version 460 core

// 播放页背景示例着色器：『光圈』
//
// 画面是一圈"频率光圈"：
// * 基线圆上按**顺时针**排布 64 个点：起点是正上方（12 点方向），角度位置对应频率
//   由低到高（正上方 = 最低频，绕一圈回到正上方前是最高的那几段），**左右不做镜像**
//   （64 个点就是 64 个频带，每个点一值）。每个点同时向**圆外**与**圆内**凸出一段尖角
//   （长度就是这一点的振幅）；相邻点的**外沿之间**、**内沿之间**都用直线连起来，
//   同一个点的内外沿之间再连一条径向线 —— 整圈看起来是一张长在圆上的"蛛网"；
// * **没有基线圆**：那圈亮线（原来的"中间那圈圆"）已经去掉，只剩蛛网与圆心光源 ——
//   频带高的地方尖角长、低的地方几乎收回到基线半径上，看起来就是一圈起伏的轮廓；
// * **亮背景自动改用"墨色"**：浅色主题 / 亮封面的背景本身很亮，白线加亮等于看不见
//   （整个发光糊在一起），所以按**背景亮度**在"加亮"与"压暗"之间平滑过渡 ——
//   亮背景把线条、光晕、中心光源都朝墨色（封面色调的暗版）混合，暗背景保持原来的加亮发光；
// * 振幅取自内置『音乐动效』提取的当前音频频谱的 64 个**频带**
//   （`spectrum.bands64.0..15`，每项 4 个连续频带写在 xyzw）。歌曲里几乎每个频带都有
//   能量，直接照着画会连成平平的一圈，所以这里先抬增益、削掉低于 `kFloor` 的底噪，
//   再取指数（`kCurve` > 1）把剩下的差异放大 —— 有的点高、有的点低，画面才有对比。
//   没有频谱数据时（uEnv.z = 0）尖角长度为 0，只剩圆心光源 —— 这时画面不动是正常的，
//   判断"没有数据"看 uEnv.z，不要看数值是不是 0。
// * **中心封面**：声明了封面（UI 项里的 `cover`）时，圆心放一张圆形封面（`uCover`），
//   正立、边缘抗锯齿，压在蛛网与光源**下面** —— 封面中心带着光晕，边缘有蛛网穿过。
//   没有封面（还没加载好 / 这首歌没有封面）时 `uCoverInfo.w` 是 0，这部分完全不动画面。
//
// 为什么是 64 个频带而不是一帧 256 个频点（`spectrum.bins.0..63`）：频点要声明成
// 64 个 vec4 成员，并且**每个像素**都要按算出来的组号去 64 个成员里挑一次
// （GLES 后端不允许用运行时下标取 uniform，只能逐组比较，见下面"写法限制"），
// 实测 1280x720 单帧渲染要多花 90 ms 以上（同一个 harness 里晶格背景只花 4 ms）——
// 播放页会明显卡。64 个频带只要 16 个成员、每个像素挑 16 组里的一组，代价小得多。
//
// uniform 约定（宿主固定填充，成员名字不能改）：
//   uParams = (目标宽, 目标高, 真实秒数, 速度)
//   uEnv.x = 是否夜间；uEnv.y = 是否有封面配色；uEnv.z = 现在是否有频谱数据
//   uColor1..uColor4 = 4 个绘制色（插件在 args 里声明来源）
//   uLine = 线条色（亮色，用于暗背景；亮背景上自动按背景亮度换成墨色）
//   uLevel = 当前响度（x = y = z = w，0~1）
//   uBands0..uBands15 = 64 个频带，低频在前，每 4 个写在 xyzw（0~1）
//   uCoverInfo = (纹理宽, 纹理高, 原图宽高比, 是否有封面) —— 声明了 cover 才有
// 纹理：uniform sampler2D uCover（方形裁剪的当前歌曲封面；画在圆心，没有封面时不参与合成）
//
// 写法限制（源码先编成 SPIR-V，运行时再按后端翻译，GLES 后端翻译成 GLSL ES 1.00）：
// * 内置函数只用浮点版：那里 `clamp` / `min` / `max` 只有浮点重载，对 int 用会翻译出
//   编不过的代码（整型钳制用 if 或三元自己写）；
// * **不能用运行时的下标取 uniform**（只允许常量下标或循环下标）：这里把 16 个频带成员
//   逐个比较（`bandGroup`）；如果哪天真要逐像素查很大的表（例如 256 个频点），代价会很高。

uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uEnv;
  vec4 uColor1;
  vec4 uColor2;
  vec4 uColor3;
  vec4 uColor4;
  vec4 uLine;
  vec4 uLevel;  vec4 uBands0;
  vec4 uBands1;
  vec4 uBands2;
  vec4 uBands3;
  vec4 uBands4;
  vec4 uBands5;
  vec4 uBands6;
  vec4 uBands7;
  vec4 uBands8;
  vec4 uBands9;
  vec4 uBands10;
  vec4 uBands11;
  vec4 uBands12;
  vec4 uBands13;
  vec4 uBands14;
  vec4 uBands15;
  vec4 uCoverInfo;
}
render_info;

// 当前歌曲封面（声明 `cover` 后由宿主每帧绑定；`uCoverInfo.w` 是 0 时表示没有封面）
uniform sampler2D uCover;

layout(location = 0) out vec4 frag_color;

const float kPi = 3.14159265;

// 圆上的点数与频带数量（一个点占一个频带，两者相等）
const int kPointCount = 64;
const int kBandCount = 64;

// 基线半径（画面短边的一半记作 1.0）：尖角就是在这个半径上向外 / 向内长的，
// 但**这个圆本身不画线**（看起来像一圈起伏的轮廓）
const float kRingRadius = 0.62;
// 向圆外 / 向内的最大尖角长度（半径 0.62 + 0.32 = 0.94，不会超出画面；
// 向内 0.30 稍小一点，把圆心让给光源）
const float kSpikeOut = 0.32;
const float kSpikeIn = 0.30;
// 振幅映射：先抬一点增益（kGain），削掉低于 kFloor 的底噪，再取指数 kCurve（> 1）
// 把剩下的差异放大 —— 歌曲里大多数频带都有能量，不做这一步整圈会连成平平的一圈。
// 想更"有棱角"就加大 kCurve（例如 3.5），想保留更多细节就减小它、或把 kFloor 调低。
const float kGain = 1.2;
const float kFloor = 0.15;
const float kCurve = 2.6;
// 线条粗细（像素）：蛛网边线按它画，光斑按它的比例画
// （换算见 main 里的 aa：aa 就是"一个像素"有多宽）
const float kLineWidth = 2.4;
const float kGlintWidth = 1.8;

// 中心封面圆盘的半径（相对短边的一半 = 1.0）：留在 0.62 的蛛网基线之内，四周有一圈空隙
const float kCoverRadius = 0.34;
// 圆盘上的封面亮度：压一点，免得盖住蛛网与光晕，也让前景文字更清楚
const float kCoverDim = 0.82;

// 取第 group 组的 4 个频带（0~1，写在 xyzw）
//
// 逐条比较见文件头"写法限制"：GLES 后端（GLSL ES 1.00）不允许用运行时下标取 uniform，
// 而组号是逐像素算出来的。这段是机械展开的，改频带数量时按同样规则增删。
vec4 bandGroup(int group) {
  if (group == 0) { return render_info.uBands0; }
  if (group == 1) { return render_info.uBands1; }
  if (group == 2) { return render_info.uBands2; }
  if (group == 3) { return render_info.uBands3; }
  if (group == 4) { return render_info.uBands4; }
  if (group == 5) { return render_info.uBands5; }
  if (group == 6) { return render_info.uBands6; }
  if (group == 7) { return render_info.uBands7; }
  if (group == 8) { return render_info.uBands8; }
  if (group == 9) { return render_info.uBands9; }
  if (group == 10) { return render_info.uBands10; }
  if (group == 11) { return render_info.uBands11; }
  if (group == 12) { return render_info.uBands12; }
  if (group == 13) { return render_info.uBands13; }
  if (group == 14) { return render_info.uBands14; }
  return render_info.uBands15;
}

// 第 index 个频带的原始值（0~1，低频在前）；下标越界时按最近的一个处理
//
// 组号（下标 / 4）与组内位置（下标 % 4）都用浮点算：不依赖整型的除法与取余在各后端
// 翻译成什么样。组内位置固定是 0~3，用三条比较挑出分量。
float bandRaw(int index) {
  int clamped = index;
  if (clamped < 0) { clamped = 0; }
  if (clamped > kBandCount - 1) { clamped = kBandCount - 1; }
  float position = float(clamped) * 0.25;
  float group = floor(position);
  float local = float(clamped) - 4.0 * group;
  vec4 value = bandGroup(int(group));
  if (local < 0.5) { return value.x; }
  if (local < 1.5) { return value.y; }
  if (local < 2.5) { return value.z; }
  return value.w;
}

// 频带振幅（0~1）：没有频谱数据时是 0（按静音处理，不用固定值）
//
// 对比度来自两处：削掉 kFloor 以下的底噪（那部分直接按 0 画，尖角收回基线），
// 剩下的再取 kCurve 次幂 —— 指数把"有点能量"和"很响"拉开，画面才有高低起伏。
float bandAmp(int index) {
  float raw = bandRaw(index) * render_info.uEnv.z;
  float v = (raw * kGain - kFloor) / (1.0 - kFloor);
  return pow(clamp(v, 0.0, 1.0), kCurve);
}

// 点到线段的距离（两个端点都是画布坐标；蛛网的每条边都是直的线段）
float distToSegment(vec2 p, vec2 a, vec2 b) {
  vec2 ab = b - a;
  float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-9), 0.0, 1.0);
  return length(p - (a + ab * t));
}

// 等边三角形的距离场（中心在原点，inRadius 是内切圆半径，尖角朝上）
float sdTriangle(vec2 p, float inRadius) {
  float d0 = dot(p, vec2(0.0, 1.0));
  float d1 = dot(p, vec2(-0.8660254, -0.5));
  float d2 = dot(p, vec2(0.8660254, -0.5));
  return max(max(d0, d1), d2) - inRadius;
}

// 一个空心小三角（参考图里散落的那些小三角）；lineWidth 与坐标同单位
float glintAt(vec2 p, vec2 center, float size, float rot, float lineWidth) {
  vec2 q = p - center;
  float s = sin(rot);
  float c = cos(rot);
  vec2 qr = vec2(q.x * c - q.y * s, q.x * s + q.y * c);
  float d = abs(sdTriangle(qr, size * 0.5));
  return 1.0 - smoothstep(0.0, max(lineWidth, 1e-4), d);
}

// 稳定随机数（同样的输入永远得到同样的值）
float hash11(float x) {
  return fract(sin(x * 127.1) * 43758.5453);
}

// 中心光源附近散落的小三角光斑：位置固定，随时间与高频能量闪动
float glints(vec2 p, float t, float treble, float lineWidth) {
  float sum = 0.0;
  for (int i = 0; i < 5; ++i) {
    float fi = float(i);
    float phase = hash11(fi + 1.7);
    float angle = (hash11(fi + 3.1) * 2.0 - 1.0) * 2.2;
    float radius = 0.16 + 0.40 * hash11(fi + 5.3);
    vec2 center = vec2(sin(angle), cos(angle)) * radius;
    float size = 0.030 + 0.030 * hash11(fi + 7.9);
    float spin = (hash11(fi + 11.3) > 0.5 ? 1.0 : -1.0)
        * (0.15 + 0.25 * hash11(fi + 9.7));
    float blink = 0.5 + 0.5 * sin(t * (0.5 + 0.8 * phase) + phase * 6.2831);
    // 平方让闪动有明暗对比，不会一直亮着
    float strength = blink * blink * (0.15 + 0.85 * treble) * 0.5;
    sum += glintAt(p, center, size, phase * 6.2831 + t * spin, lineWidth) * strength;
  }
  return sum;
}

// 4 个绘制色按"左上-右上-左下-右下"四角双线性混合（与晶格示例一致）
vec3 blendColors(vec2 uv, float dim) {
  float xBlend = smoothstep(0.0, 1.0, clamp(uv.x, 0.0, 1.0));
  float yBlend = smoothstep(0.0, 1.0, clamp(uv.y, 0.0, 1.0));
  vec3 top = mix(render_info.uColor1.rgb, render_info.uColor2.rgb, xBlend);
  vec3 bottom = mix(render_info.uColor3.rgb, render_info.uColor4.rgb, xBlend);
  return mix(top, bottom, yBlend) * dim;
}

// 中心光束：几层不同频率的角向正弦叠加，随时间缓慢转动
float beamPattern(float a, float t) {
  float s = sin(a * 6.0 + t * 0.35)
      + 0.7 * sin(a * 11.0 - t * 0.27)
      + 0.5 * sin(a * 19.0 + t * 0.19)
      + 0.35 * sin(a * 31.0 - t * 0.13);
  s = clamp(0.5 + 0.25 * s, 0.0, 1.0);
  return s * s * s;
}

void main() {
  vec2 res = max(render_info.uParams.xy, vec2(1.0));
  vec2 uv = gl_FragCoord.xy / res;
  // 以画面中心为原点、按短边归一化（短边的一半 = 1.0）
  vec2 p = (gl_FragCoord.xy - 0.5 * res) / (0.5 * min(res.x, res.y));
  // 渲染目标是左上角为原点的像素坐标（gl_FragCoord.y 越大越靠下），这里翻一下 y，
  // 让 p.y 向上为正：角度按"正上方 = 0、顺时针为正"算
  p.y = -p.y;
  float r = length(p);
  // 一个像素对应多少 r：线宽与三角描边都按它换算，窗口变大也不会变粗
  float aa = max(fwidth(r), 1e-5);

  // 角度：正上方为 0、顺时针为正（向右旋转 = 0 → pi/2 → pi → 3pi/2）
  float signedAng = atan(p.x, p.y);
  // 折到 [0, 2*pi)：64 个点从正上方开始顺时针排一圈
  float ang = signedAng < 0.0 ? signedAng + 2.0 * kPi : signedAng;

  // 频谱：没有数据时全部按 0 走（画面与不带频谱时一致）
  float spectrumOn = render_info.uEnv.z > 0.5 ? 1.0 : 0.0;
  float level = clamp(render_info.uLevel.x, 0.0, 1.0) * spectrumOn;
  // 低音 / 高音能量（中心光束与光斑用）：最低 / 最高的 16 个频带的平均，
  // 直接取前后各 4 组频带相加（不用逐个查表）
  float bass = clamp(
      dot(render_info.uBands0 + render_info.uBands1 + render_info.uBands2
          + render_info.uBands3, vec4(0.0625)), 0.0, 1.0) * spectrumOn;
  float treble = clamp(
      dot(render_info.uBands12 + render_info.uBands13 + render_info.uBands14
          + render_info.uBands15, vec4(0.0625)), 0.0, 1.0) * spectrumOn;

  // ---- 光圈轮廓：整圈 kPointCount 个点的"蛛网" ----
  //
  // 圆上有 kPointCount 个点（角度 i * delta，i = 0..kPointCount-1；0 = 正上方、
  // 顺时针转一圈，最后一个点回到正上方之前），第 i 个点的振幅就是第 i 个频带。
  // 每个点有两个半径：外沿 R + kSpikeOut * 振幅、内沿 R - kSpikeIn * 振幅；相邻点的
  // **同侧半径**用直线连起来（外连外、内连内），同一个点的内外沿之间再连一条径向线：
  // 整圈是一张闭合的蛛网（最后一段跨过正上方接回起点）。
  //
  // 本像素落在哪一段：角度除以段宽（下标先在浮点上钳制再转 int）
  float delta = 2.0 * kPi / float(kPointCount);
  int seg = int(clamp(floor(ang / delta), 0.0, float(kPointCount - 1)));
  // 下一个点：最后一个点绕回第一个点（整数运算 + if，不依赖取余）
  int next = seg + 1;
  if (next >= kPointCount) {
    next = 0;
  }
  float angA = float(seg) * delta;
  // 段宽固定：angB 就取 angA 往前一段（最后一个点的 angB = 2*pi，绕回正上方，
  // 所以**不能**用 `float(next) * delta`，那样最后一段会跳回 0）
  float angB = angA + delta;
  float ampA = bandAmp(seg);
  float ampB = bandAmp(next);
  // 本像素在这两点之间的进度（最后一个点绕回正上方，进度仍然从 angA 往前走）
  float localT = (ang - angA) / delta;
  float outA = kRingRadius + kSpikeOut * ampA;
  float outB = kRingRadius + kSpikeOut * ampB;
  float inA = kRingRadius - kSpikeIn * ampA;
  float inB = kRingRadius - kSpikeIn * ampB;

  // 本段要画的四条边：外沿弦、内沿弦、两端点的径向线；
  // 相邻段共用端点（第 i 段的外沿弦终点 = 第 i+1 段的外沿弦起点），所以整圈是连起来的
  float cosA = cos(angA);
  float sinA = sin(angA);
  float cosB = cos(angB);
  float sinB = sin(angB);
  vec2 cursor = vec2(sin(ang), cos(ang)) * r;
  vec2 vOutA = vec2(sinA, cosA) * outA;
  vec2 vOutB = vec2(sinB, cosB) * outB;
  vec2 vInA = vec2(sinA, cosA) * inA;
  vec2 vInB = vec2(sinB, cosB) * inB;
  float dOutline = min(
      min(distToSegment(cursor, vOutA, vOutB), distToSegment(cursor, vInA, vInB)),
      min(distToSegment(cursor, vInA, vOutA), distToSegment(cursor, vInB, vOutB)));

  // 本段内部的填充范围（在两端之间按角度线性插值；尖角里只有很淡的一层，让蛛网有厚度）
  float outLocal = mix(outA, outB, localT);
  float inLocal = mix(inA, inB, localT);

  // 时间：uParams.z 是真实秒数、uParams.w 是插件声明的速度（着色器要自己乘）
  float t = render_info.uParams.z * render_info.uParams.w;

  vec3 line = render_info.uLine.rgb;
  vec3 avgColor = (render_info.uColor1.rgb
      + render_info.uColor2.rgb
      + render_info.uColor3.rgb
      + render_info.uColor4.rgb) * 0.25;
  // 光源色：线条色掺一点封面配色，避免和背景完全脱开
  vec3 lightColor = mix(line, clamp(avgColor * 2.2 + 0.25, 0.0, 1.0), 0.30);

  // 背景：4 色双线性混合（夜间压暗）
  float dim = render_info.uEnv.x > 0.5 ? 0.72 : 1.0;
  float valid = render_info.uEnv.y > 0.5 ? 1.0 : 0.88;
  vec3 color = blendColors(uv, dim * valid);

  // 背景亮度决定"发光"还是"墨色"：
  // * 暗背景（夜间主题 / 暗封面）：线条与光晕**加亮**（原来的样子）；
  // * 亮背景（浅色主题 / 亮封面）：加亮等于看不见（白线糊在浅色背景上），
  //   改成把背景朝**墨色**混合 —— 线条、光晕、中心光源都变成压暗的一版；
  // 按背景亮度在两者之间过渡：过渡带窄一些（0.45~0.55），免得半亮半暗的背景
  // 拿到一个"既不够暗也不够亮"的墨色（那种背景本来就只能二选一）。
  float bgLuma = dot(color, vec3(0.299, 0.587, 0.114));
  float inkAmount = smoothstep(0.45, 0.55, bgLuma);
  // 墨色：亮背景时取封面配色的暗版（近黑但带一点画面色调），暗背景时用不到
  vec3 inkColor = mix(line, avgColor * 0.18, inkAmount);

  // 各项"光"的强度（同一份数值两种用：暗背景加亮、亮背景压暗）
  float halo = exp(-r * 3.0);
  float haloTerm = halo * (0.06 + 0.20 * level);
  // 中心光束：低音越强光束越亮，随时间缓慢转动
  float beamTerm = beamPattern(ang, t) * (0.10 + 0.55 * bass) * halo * halo;
  // 中心光源：针尖亮核 + 外扩光晕（没有频谱数据时也亮着，只是不随音乐起伏）
  float core = exp(-r * r * 520.0);
  float coreTerm = core * 2.0 + exp(-r * 10.0) * (0.18 + 0.12 * level);
  // 尖角内部：一层很淡的填充，越靠外越亮（参考图里尖角基本是空心的，所以这里压得很低）
  float span = max(outLocal - inLocal, 1e-4);
  float fillEdge = clamp((r - inLocal) / span, 0.0, 1.0);
  float inside = (r <= outLocal && r >= inLocal) ? 1.0 : 0.0;
  float fillTerm = inside * (0.03 + 0.07 * fillEdge) * (0.45 + 0.55 * level);
  // 蛛网边线：亮线；响应越大越亮，外面再套一层淡淡的光（bloom）
  // （原来那圈"基线圆"的亮线已去掉：尖角低的地方直接收在半径上，不再画那条圆线）
  float width = max(aa * kLineWidth, 0.002);
  float outline = 1.0 - smoothstep(0.0, width, dOutline);
  float bloom = exp(-dOutline * 8.0) * 0.26;
  float webTerm = outline * (0.85 + 0.35 * level) + bloom;
  // 中心附近散落的小三角光斑（跟随高频能量闪动）
  float glintTerm = glints(p, t, treble, max(aa * kGlintWidth, 0.0026));
  float litTerm = haloTerm + beamTerm + coreTerm + fillTerm + webTerm + glintTerm;

  // 中心封面圆盘：声明了 cover 且当前有封面时，在圆心放一张圆形封面（正立、边缘抗锯齿）。
  // 光源与蛛网在它上面继续叠加：封面中心带着光晕、边缘有蛛网穿过。
  // 采样坐标要翻 y（p.y 向上为正，纹理 y 向下为正），否则封面会上下颠倒。
  vec2 coverDisk = p / kCoverRadius;
  float coverDist = length(coverDisk);
  float coverAA = max(fwidth(coverDist), 1e-5);
  vec2 coverUV = vec2(coverDisk.x, -coverDisk.y) * 0.5 + 0.5;
  vec3 coverColor = texture(uCover, coverUV).rgb;
  // 没有封面时 `uCoverInfo.w` 是 0：这一次混合不去动画面（采样到的是占位纹理）
  float coverOn = render_info.uCoverInfo.w > 0.5 ? 1.0 : 0.0;
  float coverMask =
      (1.0 - smoothstep(1.0 - coverAA * 2.0, 1.0, coverDist)) * coverOn;
  color = mix(color, coverColor * kCoverDim, coverMask);

  // 暗背景：加亮（原来的发光；inkAmount = 0 时与之前完全一致）
  color += lightColor * litTerm * (1.0 - inkAmount);
  // 亮背景：把背景朝墨色混合（覆盖度就是同一份强度，钳制到 1 → 线条是干净的墨色）
  color = mix(color, inkColor, clamp(litTerm, 0.0, 1.0) * inkAmount);
  // 轻微暗角；夜间再压一点，避免大面积过曝
  color *= 1.0 - 0.20 * smoothstep(0.25, 1.35, r);
  frag_color = vec4(max(color, vec3(0.0)), 1.0);
}
