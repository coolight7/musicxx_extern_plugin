#version 460 core

// 播放页背景『模糊』（示例封面背景 · 模式一）：当前歌曲封面**预模糊**后铺满屏幕
// （再按插件声明的倍率放大），画面缓慢"四处移动"（缓慢漂移），局部偶尔出现轻微扭曲 ——
// 类似夏天地面温度高、空气被折射时看到的轻微晃动。
//
// 模糊强度与放大倍率由设置页决定（写进 UI 项的 `cover.blur` 与 `args.uZoom`），
// 扭曲波纹可以用设置页的开关关掉（`args.uRipple`）：关掉后画面只剩缓慢漂移。
//
// 封面由宿主准备（UI 项的 `cover` 声明）：解码 + 缩放 + 预模糊（sigma = 设置页的模糊程度），
// 着色器只做放大、漂移、扭曲与压暗 —— 比逐像素自己模糊便宜得多（那是每帧全屏多次采样）。
// 没有封面时 `uCoverInfo.w` 是 0，画面回退成"暗底 + 两个绘制色的缓慢光斑"；
// **不要靠采样到的颜色判断有没有封面**（那时绑定的是 1×1 透明占位纹理）。
//
// uniform 约定（宿主每帧填充，成员名不能改）：
//   uParams    = (目标宽, 目标高, 时间秒, 速度)
//   uEnv       = (是否夜间, 是否有封面配色, 是否有频谱数据, 保留)
//   uCoverInfo = (纹理宽, 纹理高, 原图宽高比, 是否有封面)
//   uGlow / uGlow2 = 封面配色映射出来的绘制色（没有分析结果时用插件声明的固定值）
//   uZoom      = x 是画面放大倍率（1~3，插件在 args 里声明；0 = 只铺满、不额外放大）
//   uRipple    = x 是热浪扭曲波纹开关（1 开 / 0 关）
// 纹理：uniform sampler2D uCover（预模糊的封面；没有封面时是 1×1 透明占位）

uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uEnv;
  vec4 uCoverInfo;
  vec4 uGlow;
  vec4 uGlow2;
  vec4 uZoom;
  vec4 uRipple;
} render_info;

uniform sampler2D uCover;

layout(location = 0) out vec4 frag_color;

// 画面放大倍率由插件在 UI 项的 `args` 里声明（成员 `uZoom`）：3 = 纹理按 3 倍放在屏幕
// 上（超出裁掉），能看到封面约 1/3 的画面；0 / 没声明时只做"铺满"（cover）不额外放大。
const float kFillOnly = 1.0;

// 漂移幅度（占屏幕宽 / 高的比例）：四处缓移，别太大（边缘露出由 clamp 兜住）
const float kDrift = 0.045;

// 热浪扭曲强度（像素，按画面短边折算）：很小，只是"微微晃一下"
const float kHeatPixels = 4.0;

// 热浪斑块的疏密、游走速度与"能看见"的门限
const float kHeatScale = 5.2;
const float kHeatSpeed = 0.32;
const float kHeatRippleSpeed = 1.45;

/// 热浪场：两列低频行波叠加（值域约 -1..1），随时间缓慢游走
float heatField(vec2 uv, float t) {
  float a = sin(
      uv.x * kHeatScale + t * kHeatSpeed +
      sin(uv.y * (kHeatScale * 0.8) - t * (kHeatSpeed * 0.64)) * 1.6);
  float b = sin(
      uv.y * (kHeatScale * 1.2) - t * (kHeatSpeed * 0.79) +
      sin(uv.x * (kHeatScale * 0.7) + t * (kHeatSpeed * 0.52)) * 1.2);
  return (a + b) * 0.5;
}

void main() {
  vec2 res = max(render_info.uParams.xy, vec2(1.0));
  // 时间：真实秒数 × 插件声明的速率（速度由 UI 项里的 speed 决定）
  float t = render_info.uParams.z * max(render_info.uParams.w, 0.0001);
  vec2 uv = gl_FragCoord.xy / res;

  // 有没有封面：看信息位（采样到的颜色不能用来判断）
  float coverOn = render_info.uCoverInfo.w > 0.5 ? 1.0 : 0.0;
  // 热浪扭曲的开关（设置页可关；关掉后画面只剩缓慢漂移）
  float heatOn = render_info.uRipple.x > 0.5 ? 1.0 : 0.0;

  // 1) 放大：纹理在屏幕上的**总体**放大倍率（不小于"铺满"所需，否则会露出边缘）
  vec2 texSize = max(render_info.uCoverInfo.xy, vec2(1.0));
  float fillScale = max(res.x / texSize.x, res.y / texSize.y);
  float coverScale = max(fillScale, max(render_info.uZoom.x, kFillOnly));
  // 屏幕 uv → 纹理 uv 的步长：采样范围 = 屏幕尺寸 / 纹理在屏幕上的尺寸。
  // **这里是逐分量相除**（不是相乘）：用倒数写会让宽高比不同的屏幕把画面拉伸变形。
  vec2 uvStep = res / (texSize * coverScale);

  // 2) 四处移动：两条周期不同的椭圆轨迹叠加（慢速、不重复地游走）
  vec2 drift = vec2(
      sin(t * 0.083) * 0.6 + sin(t * 0.031 + 1.7) * 0.4,
      cos(t * 0.071 + 0.6) * 0.6 + cos(t * 0.043 + 2.9) * 0.4);
  vec2 driftUV = drift * kDrift;

  // 3) 局部热浪：只在场的波峰附近出现（"偶尔某些地方"），幅度只有几像素
  float field = heatField(uv + driftUV * 0.5, t);
  float heatMask = smoothstep(0.30, 0.95, field);
  float ripple = sin(
      dot(uv, vec2(31.0, 19.0)) + t * kHeatRippleSpeed + field * 4.6);
  vec2 heatUV = vec2(1.0, 0.4) * (ripple * heatMask * kHeatPixels * heatOn) / res;

  // 4) 采样：漂移与热浪叠加后夹在纹理内（采样的地址模式本身也是 clampToEdge）
  vec2 sampleUV = clamp(
      (uv - 0.5) * uvStep + 0.5 + driftUV + heatUV, 0.0, 1.0);
  vec3 coverColor = texture(uCover, sampleUV).rgb;

  // 5) 没有封面时的底：暗色竖向渐变 + 两个绘制色的缓慢光斑（很淡，只做气氛）
  vec3 fallback = mix(
      vec3(0.05, 0.055, 0.07), vec3(0.095, 0.105, 0.135), uv.y);
  vec2 spotA = vec2(
      0.32 + 0.16 * sin(t * 0.11), 0.30 + 0.10 * cos(t * 0.079));
  vec2 spotB = vec2(
      0.70 - 0.13 * cos(t * 0.093), 0.68 + 0.12 * sin(t * 0.067));
  float glowA = smoothstep(0.85, 0.0, distance(uv, spotA));
  float glowB = smoothstep(0.75, 0.0, distance(uv, spotB));
  fallback += render_info.uGlow.rgb * glowA * 0.09 +
      render_info.uGlow2.rgb * glowB * 0.07;

  vec3 color = mix(fallback, coverColor, coverOn);

  // 6) 夜间压暗（与内置背景同一份约定：uEnv.x = 是否夜间）
  float dim = render_info.uEnv.x > 0.5 ? 0.74 : 1.0;
  color *= dim;

  // 轻微暗角：把视线收在画面中间
  float vignette = distance(uv, vec2(0.5, 0.5));
  color *= 1.0 - 0.16 * smoothstep(0.30, 1.30, vignette);

  frag_color = vec4(max(color, vec3(0.0)), 1.0);
}
