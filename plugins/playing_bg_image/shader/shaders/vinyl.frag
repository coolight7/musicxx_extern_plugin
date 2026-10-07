#version 460 core

// 播放页背景『黑胶』（示例封面背景 · 模式三）：封面主色铺底，画面中间摆一张黑胶唱片 ——
// 盘面是近黑的底 + 细密的音轨纹路 + 来自左上方的柔光与外缘倒角高光；唱片中心是当前歌曲的
// 封面（圆裁、缓慢旋转，像唱片在转）；盘沿上方一支白色唱臂（转轴 + 圆弧臂管 + 唱头壳，
// 周围一层很软的淡阴影），唱头贴着盘沿。
//
// 版式（单位都是"唱片半径 R"，原点在唱片中心，y 向下；屏上 y 与 gl_FragCoord 一致）：
//   · R = min(短边 × 0.41, 画面高 × 0.33)：前者决定唱片大小（竖屏时约等于 0.41 倍屏宽，
//     与参考图一致），后者保证上方的唱臂留在画面里面；
//   · 唱片中心在画面中心下方 0.22R（上方要给唱臂留位置）；
//   · 0.00 ~ 0.70R 是封面圆（唱片标签），0.70R ~ 1.00R 是音轨区；
//   · 唱臂是白色轻拟物的一条**折线**臂管：顶端的圆形转轴在 (0.12,-1.30)（盘外上方偏右），
//     第一段**长段**（0.72R、约 −37° 的缓坡）伸到肘部 (0.70,-0.87)，折约 40°（肘部 0.10R
//     圆角过渡）后接第二段**短段**（0.24R、约 −77°，两段长度比约 3:1），末端 (0.755,-0.64)
//     接一小段**贴着盘沿**的唱头壳（到 (0.838,-0.542)）；
//     形状外面垫着两层很软的边：贴着一圈淡白光晕 + 一层偏右下的柔影。
//
// 动画：**只有封面在转**（1 圈 = kSpinSeconds 秒，再乘插件声明的 speed）。音轨纹路与反光都是
// 旋转对称的，盘面自己转起来看不出来；唱臂不动（参考图里也是不动的）。
//
// uniform 约定（宿主每帧填充，成员名不能改）：
//   uParams    = (目标宽, 目标高, 时间秒, 速度)
//   uEnv       = (是否夜间, 是否有封面配色, 是否有频谱数据, 保留)
//   uCoverInfo = (纹理宽, 纹理高, 原图宽高比, 是否有封面)
//   uBg        = 底色（封面主色，来自 musicxx.icon.themeMapping.0）
//   uBg2       = 底色的第二个色（封面第二绘制色，来自 musicxx.icon.themeMapping.1）
//   uArm       = 唱臂的颜色（近白；淡光晕与柔影都按它和底色算，见 kArmHalo* / kArmShadow*）
// 纹理：uniform sampler2D uCover（**正方形**封面，渲染项里声明 cover.square = true；
// 没有封面时绑的是 1×1 透明占位纹理，这时用 uCoverInfo.w 判断、改画一个两色渐变的标签）。

uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uEnv;
  vec4 uCoverInfo;
  vec4 uBg;
  vec4 uBg2;
  vec4 uArm;
} render_info;

uniform sampler2D uCover;

layout(location = 0) out vec4 frag_color;

// 唱片半径：按短边取，再受画面高度约束（唱臂要留在画面里，见文件头）
const float kDiscScale = 0.41;
const float kDiscMaxOfHeight = 0.33;

// 唱片中心相对画面中心下移多少个 R
const float kDiscDrop = 0.22;

// 封面圆（唱片标签）的半径，相对唱片半径
const float kLabelR = 0.70;

// 音轨纹路的疏密与强度：ringPhase 每 1 个 R 绕 kGrooveRings 圈，另外两档更细 / 更粗
// **三档都是旋转对称的**（只跟半径有关），所以唱片转起来时纹路不会"跟着转"
const float kGrooveRings = 48.0;
const float kGrooveFine = 2.13;
const float kGrooveWide = 0.31;
const float kGrooveAmp = 0.030;
const float kGrooveFineAmp = 0.016;
const float kGrooveWideAmp = 0.022;

// 盘面基色（近黑，稍微偏冷）
const vec3 kDiscBase = vec3(0.048, 0.050, 0.058);

// 光：从左上方来；柔光（宽）与外缘倒角高光的强度
const vec2 kLightDir = vec2(-0.30, -0.95);
const float kSheen = 0.085;
const float kSpec = 0.10;
const float kRim = 0.10;

// 封面转一圈的秒数（speed = 1 时；speed 变了转速按比例变）
const float kSpinSeconds = 30.0;

// 唱臂（白色轻拟物）：一条**折线**臂管（像手臂那样在肘部折一下，折角处用一段圆角过渡）+
// 顶端的圆形转轴 + 末端一小段贴着盘沿的唱头壳；形状外面垫两层很软的边：贴着一圈淡白光晕
// 与一层偏右下的柔影。
//
// 折线的三个关键点（R 为单位，原点在唱片中心，y 向下）：
//   转轴 (0.12,-1.30) → 肘 (0.70,-0.87) → 唱头接点 (0.755,-0.64)
// 第一段是**长段**（0.72R，约 −37° 的缓坡伸出去），第二段是**短段**（0.24R，约 −77° 折下来
// 接唱头），两段长度比约 **3:1**、折角约 40° —— 真唱臂也是"一根长臂管 + 末端一小段偏角的
// 唱头"，这样看着才像在放唱片。
// 肘部按半径 0.10R 做圆角过渡，过渡段用一条二次贝塞尔近似（与真圆弧差 ≈ 0.001R，看不出来；
// GLSL ES 1.00 里不能定义常量数组，所以采样点直接写成一个个常量）。
const vec2 kArmPivot = vec2(0.12, -1.30);
const vec2 kArmElbow = vec2(0.70, -0.87);
const vec2 kArmPath1 = vec2(0.3954, -1.0959);   // 长段中点
const vec2 kArmCornerA = vec2(0.6708, -0.8917); // 圆角起点
const vec2 kArmCornerM = vec2(0.6948, -0.8666); // 圆角中点
const vec2 kArmCornerB = vec2(0.7085, -0.8346); // 圆角终点
const vec2 kArmPath2 = vec2(0.7318, -0.7373);   // 短段中点
const float kArmHalf = 0.026;         // 臂管半宽（屏幕上约 8 像素）
const float kArmPivotR = 0.086;       // 转轴圆盘的半径
const float kArmPivotInnerR = 0.047;  // 转轴里的小圆（微微发灰，像轴承）
// 唱头壳：一小段（比臂管粗一点）贴着盘沿，**别做长** —— 做长了就成了末端一个大疙瘩
const vec2 kHeadFrom = vec2(0.755, -0.64);
const vec2 kHeadTo = vec2(0.838, -0.542);
const float kHeadHalf = 0.042;
const float kArmHaloWidth = 0.075;    // 淡光晕的宽度（R）
const float kArmHaloStrength = 0.30;  // 淡光晕的强度
const vec2 kArmShadowOffset = vec2(0.020, 0.052);  // 柔影相对唱臂的偏移（光在左上）
const float kArmShadowWidth = 0.085;               // 柔影往外散开的宽度（R）
const float kArmShadowStrength = 0.28;             // 柔影的浓度

/// 点到线段的距离场（线段两端是圆头），w = 半宽
float sdSegment(vec2 p, vec2 a, vec2 b, float w) {
  vec2 pa = p - a;
  vec2 ba = b - a;
  float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
  return length(pa - ba * h) - w;
}

/// 点到圆的距离场
float sdCircle(vec2 p, vec2 c, float r) {
  return length(p - c) - r;
}

/// 唱臂的距离场（负值 = 在臂里面）
///
/// 三件取并集：顶端的圆形转轴、贴着盘沿的唱头壳、以及把它们连起来的**折线**臂管。
/// 臂管把中心线采样成若干圆头短段：直线段各取两段、肘部圆角取三段 —— 每段都是圆头，
/// 接缝看不出来，折角处则是圆润的过渡（几段直线硬拼会看出折角是尖的）。
float sdTonearm(vec2 p) {
  float d = sdCircle(p, kArmPivot, kArmPivotR);
  d = min(d, sdSegment(p, kHeadFrom, kHeadTo, kHeadHalf));
  d = min(d, sdSegment(p, kArmPivot, kArmPath1, kArmHalf));
  d = min(d, sdSegment(p, kArmPath1, kArmCornerA, kArmHalf));
  d = min(d, sdSegment(p, kArmCornerA, kArmCornerM, kArmHalf));
  d = min(d, sdSegment(p, kArmCornerM, kArmCornerB, kArmHalf));
  d = min(d, sdSegment(p, kArmCornerB, kArmPath2, kArmHalf));
  // 臂管末端就是唱头壳的接点：两段接在一起
  d = min(d, sdSegment(p, kArmPath2, kHeadFrom, kArmHalf));
  return d;
}

void main() {
  vec2 res = max(render_info.uParams.xy, vec2(1.0));
  // 时间：真实秒数 × 插件声明的速率（速率由 UI 项里的 speed 决定）
  float t = render_info.uParams.z * max(render_info.uParams.w, 0.0001);
  vec2 screenUV = gl_FragCoord.xy / res;

  float night = render_info.uEnv.x > 0.5 ? 1.0 : 0.0;
  float coverOn = render_info.uCoverInfo.w > 0.5 ? 1.0 : 0.0;

  // 1) 版式：唱片半径与中心，q 是"以唱片中心为原点、以唱片半径为单位"的坐标
  float minSide = min(res.x, res.y);
  float discR = max(min(minSide * kDiscScale, res.y * kDiscMaxOfHeight), 8.0);
  vec2 discCenter = vec2(res.x * 0.5, res.y * 0.5 + discR * kDiscDrop);
  vec2 q = (gl_FragCoord.xy - discCenter) / discR;
  float rn = length(q);
  // 边缘抗锯齿的宽度：约 1.5 像素，换算到 R 单位
  float aa = 1.5 / discR;

  // 2) 底：封面主色，中间稍亮、四周压暗（第二色只掺一点，避免底色跑色）
  float fall = smoothstep(0.10, 1.90, rn);
  vec3 deep = mix(render_info.uBg.rgb * 0.70, render_info.uBg2.rgb, 0.35);
  vec3 color = mix(render_info.uBg.rgb, deep, fall);
  // 唱片"压"在底上的柔影：紧贴盘边的一圈压暗
  color *= 1.0 - 0.20 * (1.0 - smoothstep(1.0, 1.38, rn));

  // 3) 唱片
  float discMask = 1.0 - smoothstep(-aa, aa, rn - 1.0);
  if (discMask > 0.0) {
    // 3.1) 盘面：近黑的底 + 三档同心音轨纹路
    float ringPhase = rn * 6.28318 * kGrooveRings;
    float grooves = 0.5 + 0.5 * sin(ringPhase);
    float fine = 0.5 + 0.5 * sin(ringPhase * kGrooveFine + 1.7);
    float wide = 0.5 + 0.5 * sin(ringPhase * kGrooveWide + 0.4);
    vec3 vinyl = kDiscBase + vec3(
        kGrooveAmp * grooves + kGrooveFineAmp * fine + kGrooveWideAmp * wide) * 0.85;

    // 3.2) 盘面的光：朝光源的那一侧亮一点（宽柔光 + 窄高光），中心区域不起作用（那里是封面）
    vec2 dir = q / max(rn, 0.0001);
    float ndl = dot(dir, normalize(kLightDir));
    float lift = smoothstep(0.02, 0.26, rn);
    vinyl += vec3(
        kSheen * pow(max(ndl, 0.0), 2.2) + kSpec * pow(max(ndl, 0.0), 26.0)) * lift;
    // 外缘的倒角高光（只在朝光的一侧）
    vinyl += vec3(kRim) * smoothstep(0.960, 1.0, rn) * smoothstep(-0.15, 0.75, ndl);

    // 3.3) 标签（封面圆）：外圈一道柔影，边缘一条细线
    float dLabel = rn - kLabelR;
    float labelMask = 1.0 - smoothstep(-aa, aa, dLabel);
    float labelShadow = (1.0 - labelMask) * (1.0 - smoothstep(0.0, 0.10, dLabel));
    vinyl *= 1.0 - 0.45 * labelShadow;
    float edgeLine = exp(-pow(dLabel / 0.012, 2.0));

    // 3.4) 封面：圆裁 + 缓慢旋转（纹理是正方形的，圆内正好是它的内切圆）；
    //      没有封面时画一个两色渐变的标签（不能拿"采样到的颜色"判断有没有封面）
    float spin = t * (6.28318 / kSpinSeconds);
    float spinCos = cos(spin);
    float spinSin = sin(spin);
    vec2 coverPos = mat2(spinCos, -spinSin, spinSin, spinCos) * (q / kLabelR);
    vec3 label = mix(
        render_info.uBg2.rgb * 0.75,
        render_info.uBg.rgb * 0.85,
        clamp(0.5 - 0.5 * coverPos.y, 0.0, 1.0));
    if (coverOn > 0.5) {
      label = texture(uCover, clamp(coverPos * 0.5 + 0.5, 0.0, 1.0)).rgb;
    }
    label *= 1.0 - 0.30 * edgeLine;
    label += vec3(1.0) * 0.12 * edgeLine * max(ndl, 0.0);

    color = mix(color, mix(vinyl, label, labelMask), discMask);
  }

  // 4) 唱臂：白色轻拟物 —— 先铺两层很软的边（贴着一圈淡白光晕 + 一层偏右下的柔影），
  //    再画主体与细小结构（转轴里的小圆、唱头壳上的细槽与接缝）
  if (q.y < -0.38 && q.x > -0.02) {
    float armSd = sdTonearm(q);
    float armAA = 1.2 / discR;
    // 柔影：同一份距离场在偏移位置再算一次，往外软软地衰减（不是硬边）
    float shadowSd = sdTonearm(q - kArmShadowOffset);
    float shadowMask = 1.0 - smoothstep(-armAA, kArmShadowWidth, shadowSd);
    color = mix(color, color * 0.70, shadowMask * kArmShadowStrength);
    // 淡光晕：贴着形状的一圈白，往外散开（拟物的"亮边"）
    float haloMask = 1.0 - smoothstep(0.0, kArmHaloWidth, armSd);
    color = mix(color, mix(color, vec3(1.0), 0.55), haloMask * kArmHaloStrength);

    // 主体：近白，靠盘面的一头稍微压一点（深色盘上的白件别过曝）
    vec3 armWhite = mix(render_info.uArm.rgb, render_info.uBg.rgb, 0.05);
    armWhite = mix(armWhite, armWhite * 0.94, smoothstep(-1.20, -0.55, q.y));
    // 唱头壳上的两条细槽 + 与臂管之间的接缝（都顺着唱头的方向）
    //
    // **只在唱头那一段里**：这两条细槽是"到一条无限长直线"的距离算出来的，不限制范围
    // 就会一路画到臂管和转轴上（白色件上出现两道横穿全臂的灰线）。
    vec2 headDir = normalize(kHeadTo - kHeadFrom);
    vec2 headPerp = vec2(-headDir.y, headDir.x);
    vec2 headRel = q - kHeadFrom;
    float headAcross = dot(headRel, headPerp);
    float headAlong = dot(headRel, headDir);
    float headLen = length(kHeadTo - kHeadFrom);
    float inHead = smoothstep(-0.012, 0.012, headAlong) *
        (1.0 - smoothstep(headLen - 0.012, headLen + 0.012, headAlong));
    float slot = (1.0 - smoothstep(0.004, 0.010, abs(abs(headAcross) - 0.016))) *
        inHead;
    float seam = 1.0 - smoothstep(0.004, 0.011, abs(headAlong - 0.022));
    vec3 armBody = mix(armWhite, armWhite * 0.42, max(slot, seam) * 0.75);
    // 转轴里的小圆（微微发灰，像轴承）
    float hubMask = 1.0 - smoothstep(
        -armAA, armAA, sdCircle(q, kArmPivot, kArmPivotInnerR));
    armBody = mix(armBody, mix(armWhite, vec3(0.74, 0.78, 0.84), 0.5), hubMask);
    color = mix(color, armBody, 1.0 - smoothstep(-armAA, armAA, armSd));
  }

  // 5) 夜间压暗（与内置背景同一份约定：uEnv.x = 是否夜间）与轻微暗角
  color *= mix(1.0, 0.86, night);
  float vignette = distance(screenUV, vec2(0.5, 0.5));
  color *= 1.0 - 0.16 * smoothstep(0.30, 1.30, vignette);

  frag_color = vec4(max(color, vec3(0.0)), 1.0);
}
