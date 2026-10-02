#version 460 core

// 播放页背景示例着色器：『光圈』
//
// 画面是一圈"频率光圈"：
// * 细亮线画出的圆是基线（半径固定），圆心是光源：高亮核心 + 光晕 + 缓慢转动的光束；
// * 基线圆上按**左右对称**的方式排布 16 个频率点：从正上方（12 点方向）往下，角度位置
//   对应频率由低到高，左右两侧互为镜像。每个点同时向**圆外**与**圆内**凸出一段尖角
//   （长度就是这一帧该频带的振幅）；相邻点的**外沿之间**、**内沿之间**都用直线连起来，
//   同一个点的内外沿之间再连一条径向线 —— 整圈看起来是一张长在圆上的"蛛网"：
//   一圈尖角，尖与尖之间用直线收口（参考图用的就是这个形状），
//   而不是每一段各自画成一个圆环扇形（那样看起来像一圈圆盘）；
// * 振幅取自内置『音乐动效』提取的当前音频频谱（`spectrum.bands.0..3`）。没有频谱数据时
//   （uEnv.z = 0）尖角长度为 0，只剩基线圆与中心光源 —— 这时画面不动是正常的，
//   判断"没有数据"看 uEnv.z，不要看数值是不是 0。
//
// uniform 契约（宿主固定填充，成员名字不能改）：
//   uParams = (目标宽, 目标高, 真实秒数, 速度)
//   uEnv.x = 是否夜间；uEnv.y = 是否有封面配色；uEnv.z = 现在是否有频谱数据
//   uColor1..uColor4 = 4 个绘制色（插件在 args 里声明来源）
//   uLine = 线条色（亮色）
//   uLevel = 当前响度（x = y = z = w，0~1）
//   uBands0..uBands3 = 16 个频带，低频在前，每 4 个写在 xyzw（0~1）

uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uEnv;
  vec4 uColor1;
  vec4 uColor2;
  vec4 uColor3;
  vec4 uColor4;
  vec4 uLine;
  vec4 uLevel;
  vec4 uBands0;
  vec4 uBands1;
  vec4 uBands2;
  vec4 uBands3;
}
render_info;

layout(location = 0) out vec4 frag_color;

const float kPi = 3.14159265;

// 基线圆半径（画面短边的一半记作 1.0）
const float kRingRadius = 0.62;
// 频率点向圆外 / 向内的最大尖角长度（都在基线的振幅方向上量：半径 0.62 + 0.32 = 0.94，
// 不会超出画面；向内 0.30 稍小一点，把圆心让给光源，也留出"蛛网"向内收的空间）
const float kSpikeOut = 0.32;
const float kSpikeIn = 0.30;
// 频带振幅的增益与曲线：归一化频带多数时候取不满，抬一点画面才有起伏
const float kGain = 1.35;
const float kCurve = 0.8;
// 频带数量（与宿主写入的 4 个 vec4 一致）
const int kBandCount = 16;

// 取 vec4 的第 k 个分量
float pick4(vec4 v, int k) {
  if (k == 0) {
    return v.x;
  }
  if (k == 1) {
    return v.y;
  }
  if (k == 2) {
    return v.z;
  }
  return v.w;
}

// 把频带下标限制在 [0, kBandCount - 1]（越界时按最近的频带处理）
//
// 不要在这里用 clamp / min / max 的**整型**版本：源码会先被编成 SPIR-V，运行时再按后端
// 翻译（GLES 后端翻译成 GLSL ES 1.00），而 GLSL ES 1.00 只有浮点版本的重载 —— 整型
// clamp 会让翻译出来的着色器编不过，表现为背景"渲染失败"然后被停用。用 if 自己写。
int clampBandIndex(int index) {
  if (index < 0) {
    return 0;
  }
  if (index > kBandCount - 1) {
    return kBandCount - 1;
  }
  return index;
}

// 第 index 个频带的原始值（0~1，低频在前）；越界时按最近的频带处理
float bandRaw(int index) {
  int i = clampBandIndex(index);
  if (i < 4) {
    return pick4(render_info.uBands0, i);
  }
  if (i < 8) {
    return pick4(render_info.uBands1, i - 4);
  }
  if (i < 12) {
    return pick4(render_info.uBands2, i - 8);
  }
  return pick4(render_info.uBands3, i - 12);
}

// 频带振幅（0~1）：没有频谱数据时是 0（按静音处理，不用固定值兜底）
float bandAmp(int index) {
  float raw = bandRaw(index) * render_info.uEnv.z;
  return pow(clamp(raw * kGain, 0.0, 1.0), kCurve);
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

// 4 个绘制色按"左上-右上-左下-右下"四角双线性混合（与晶格示例同一口径）
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
  // 让 p.y 向上为正：后面按"正上方（12 点方向）= 最低频"算角度
  p.y = -p.y;
  float r = length(p);
  // 一个像素对应多少 r：线宽与三角描边都按它换算，窗口变大也不会变粗
  float aa = max(fwidth(r), 1e-5);

  // 角度：正上方为 0、顺时针为正；取绝对值后左右两侧互为镜像
  float signedAng = atan(p.x, p.y);
  float ang = abs(signedAng);

  // 频谱：没有数据时全部按 0 走（画面与不带频谱时一致）
  float spectrumOn = render_info.uEnv.z > 0.5 ? 1.0 : 0.0;
  float level = clamp(render_info.uLevel.x, 0.0, 1.0) * spectrumOn;
  float bass = clamp(dot(render_info.uBands0, vec4(0.25)), 0.0, 1.0) * spectrumOn;
  float treble = clamp(dot(render_info.uBands3, vec4(0.25)), 0.0, 1.0) * spectrumOn;

  // ---- 光圈轮廓：半圈 kBandCount 段的"蛛网" ----
  //
  // 半圈上有 kBandCount + 1 个频率点（角度 i * delta，i = 0..kBandCount；0 = 正上方、
  // pi = 正下方），第 i 个点的振幅就是第 i 个频带；每个点因此有两个半径：外沿
  // R + kSpikeOut * 振幅、内沿 R - kSpikeIn * 振幅。相邻点的**同侧半径**用直线连起来
  // （外连外、内连内），同一个点的内外沿之间再连一条径向线：整圈是一张左右对称的蛛网，
  // 每个频率点向内外各凸出一个尖，尖与尖之间由直线收口（就是参考图的形状）。
  //
  // 本像素落在哪一段：角度除以段宽（下标先在浮点上钳制再转 int，见 clampBandIndex 的说明）
  float delta = kPi / float(kBandCount);
  int seg = int(clamp(floor(ang / delta), 0.0, float(kBandCount - 1)));
  float angA = float(seg) * delta;
  float angB = float(seg + 1) * delta;
  float ampA = bandAmp(seg);
  float ampB = bandAmp(seg + 1);
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
  float localT = (ang - angA) / delta;
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

  // 背景：4 色双线性混合（夜间压暗），中心方向整体变亮（光源在画面中心）
  float dim = render_info.uEnv.x > 0.5 ? 0.72 : 1.0;
  float valid = render_info.uEnv.y > 0.5 ? 1.0 : 0.88;
  vec3 color = blendColors(uv, dim * valid);
  float halo = exp(-r * 3.0);
  color += lightColor * halo * (0.06 + 0.20 * level);

  // 中心光束：低音越强光束越亮，随时间缓慢转动
  color += lightColor * beamPattern(signedAng, t) * (0.10 + 0.55 * bass)
      * halo * halo;

  // 中心光源：针尖亮核 + 外扩光晕（没有频谱数据时也亮着，只是不随音乐起伏）
  float core = exp(-r * r * 520.0);
  color += lightColor * core * 2.0;
  color += lightColor * exp(-r * 10.0) * (0.18 + 0.12 * level);

  // 尖角内部：一层很淡的填充，越靠外越亮（参考图里尖角基本是空心的，所以这里压得很低）
  float span = max(outLocal - inLocal, 1e-4);
  float fillEdge = clamp((r - inLocal) / span, 0.0, 1.0);
  float inside = (r <= outLocal && r >= inLocal) ? 1.0 : 0.0;
  color += lightColor * inside * (0.03 + 0.07 * fillEdge) * (0.45 + 0.55 * level);

  // 基线圆与蛛网边线：细亮线；响应越大越亮，外面再套一层淡淡的光（bloom）
  float width = max(aa * 1.1, 0.0012);
  float outline = 1.0 - smoothstep(0.0, width, dOutline);
  float baseLine = 1.0 - smoothstep(0.0, width * 0.85, abs(r - kRingRadius));
  float bloom = exp(-dOutline * 12.0) * 0.22;
  color += lightColor * (outline * (0.85 + 0.35 * level) + baseLine * 0.75 + bloom);

  // 中心附近散落的小三角光斑（跟随高频能量闪动）
  color += lightColor * glints(p, t, treble, max(aa * 1.2, 0.0015));

  // 轻微暗角；夜间再压一点，避免大面积过曝
  color *= 1.0 - 0.20 * smoothstep(0.25, 1.35, r);
  frag_color = vec4(max(color, vec3(0.0)), 1.0);
}
