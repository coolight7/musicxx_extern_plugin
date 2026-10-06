#version 460 core

// 播放页背景『渐变贴边』（示例封面背景 · 模式二）：封面主色做渐变底，清晰封面贴着屏幕的
// 边缘铺一片（自由边模糊渐隐），最后按主题叠一层亮暗遮罩。
//
//   · 竖屏（res.y >= res.x）：封面贴上、左、右三条边 —— 占满宽度，高度取
//     「按封面比例」与「屏幕高度 × 图片占比」里较大的那个（封面很扁时也占得满），
//     下边缘（自由边）模糊并渐隐到渐变底；
//   · 横屏：封面贴左、上、下三条边 —— 占满高度，宽度同样取两者的较大值，
//     右边缘（自由边）同样处理。
//
// **图片占比（`uShare`）** 就是这个"至少占屏幕多少"：调大它图片占比更多、渐变底更少
// （图片按卡片铺满、超出裁掉），调小则更像一张贴边的卡片。
// 放大倍数（1~3）只改取景：数值越大，卡片里看到的封面越局部（居中裁剪，铺满卡片的部分不变），
// 卡片本身贴住的边与尺寸不跟着变 —— 否则"贴着边缘"就不成立了。
// 热浪扭曲波纹开关打开时，封面采样位置每帧有几像素的偏移，渐变底也跟着轻微起伏。
//
// uniform 约定（宿主每帧填充，成员名不能改）：
//   uParams    = (目标宽, 目标高, 时间秒, 速度)
//   uEnv       = (是否夜间, 是否有封面配色, 是否有频谱数据, 保留)
//   uCoverInfo = (纹理宽, 纹理高, 原图宽高比, 是否有封面)
//   uBase      = 封面主色（渐变亮端）
//   uBase2     = 封面的暗色（渐变暗端）
//   uMask      = 主题背景色（亮暗遮罩的颜色：浅色主题是浅色、深色主题是深色）
//   uZoom      = x 是放大倍数（1~3）
//   uShare     = x 是"图片至少占屏幕这个比例"（0.45~0.95；0 = 只用封面比例决定卡片大小）
//   uRipple    = x 是热浪扭曲波纹开关（1 开 / 0 关）
// 纹理：uniform sampler2D uCover（清晰封面；没有封面时是 1×1 透明占位，这时只画渐变底）

uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uEnv;
  vec4 uCoverInfo;
  vec4 uBase;
  vec4 uBase2;
  vec4 uMask;
  vec4 uZoom;
  vec4 uShare;
  vec4 uRipple;
} render_info;

uniform sampler2D uCover;

layout(location = 0) out vec4 frag_color;

// 封面比例的可用范围（极端的宽/高比不让卡片变得过大或过小）
const float kAspectMin = 0.45;
const float kAspectMax = 3.0;

// 自由边：模糊从卡片自由方向的这个位置开始变强，渐隐从更靠边的地方开始
const float kFreeBlurStart = 0.35;
const float kFreeFadeStart = 0.72;

// 自由边的模糊半径（像素，卡片内）与模糊结果最多占多少
const float kEdgeBlurPixels = 12.0;
const float kEdgeBlurWeight = 0.85;

// 热浪强度（像素）与斑块的疏密、游走速度（与『模糊热浪』同一份场，观感一致）
const float kHeatPixels = 5.0;
const float kHeatScale = 5.2;
const float kHeatSpeed = 0.32;
const float kHeatRippleSpeed = 1.45;

// 亮暗遮罩的浓度：白昼与夜间各一份（夜间铺得更重一些）
const float kMaskDay = 0.22;
const float kMaskNight = 0.34;

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
  float t = render_info.uParams.z * max(render_info.uParams.w, 0.0001);
  vec2 uv = gl_FragCoord.xy / res;

  float night = render_info.uEnv.x > 0.5 ? 1.0 : 0.0;
  float coverOn = render_info.uCoverInfo.w > 0.5 ? 1.0 : 0.0;
  float rippleOn = render_info.uRipple.x > 0.5 ? 1.0 : 0.0;

  // 热浪场：只在波峰附近出现（"偶尔某些地方"）
  float field = heatField(uv, t);
  float heatMask = smoothstep(0.25, 0.95, field);
  float rippleWave = sin(
      dot(uv, vec2(31.0, 19.0)) + t * kHeatRippleSpeed + field * 4.6);
  float rippleAmount = rippleWave * heatMask * rippleOn;

  // 1) 渐变底：封面主色 → 封面暗色的斜向渐变（夜间两端都压暗一点）
  float g = clamp(uv.x * 0.40 + uv.y * 0.60 + rippleAmount * 0.03, 0.0, 1.0);
  vec3 bright = mix(
      render_info.uBase.rgb, vec3(1.0), (1.0 - night) * 0.22);
  vec3 deep = mix(
      render_info.uBase2.rgb, vec3(0.0), night * 0.30 + (1.0 - night) * 0.10);
  vec3 color = mix(bright, deep, smoothstep(0.0, 1.0, g));

  // 2) 卡片尺寸：竖屏占满宽度、横屏占满高度；自由方向取「按封面比例」与「屏幕 × uShare」里大的那个
  //    —— uShare 是"图片至少占屏幕这个比例"：调大它图片占比更多（图片按卡片铺满、超出裁掉），
  //    封面本身很扁/很长时仍按封面比例给足空间（不会因为比例被压成一条）
  float aspect = clamp(render_info.uCoverInfo.z, kAspectMin, kAspectMax);
  float share = clamp(render_info.uShare.x, 0.0, 1.0);
  bool portrait = res.y >= res.x;
  float free = portrait
      ? max(res.x / aspect, res.y * share)
      : max(res.y * aspect, res.x * share);
  vec2 card = portrait
      ? vec2(res.x, min(free, res.y))
      : vec2(min(free, res.x), res.y);
  card = max(card, vec2(1.0));

  // 卡片范围掩码：范围外的像素不画封面（与"没有封面"一样只剩渐变底）
  float inside = (1.0 - step(card.x, gl_FragCoord.x)) *
      (1.0 - step(card.y, gl_FragCoord.y));

  // 卡片贴屏幕左上角，所以渲染像素坐标就是卡片内的局部坐标（渲染目标 y 向下）
  vec2 localUV = gl_FragCoord.xy / card;
  // 0 = 贴住屏幕的那几条边，1 = 自由边（竖屏是下边、横屏是右边）
  float freeCoord = portrait ? localUV.y : localUV.x;

  // 3) 取景：铺满卡片所需的倍率 × 放大倍数，居中裁剪（放大倍数越大看到越局部）
  vec2 texSize = max(render_info.uCoverInfo.xy, vec2(1.0));
  float fillScale = max(card.x / texSize.x, card.y / texSize.y);
  float scale = fillScale * max(render_info.uZoom.x, 1.0);
  // 屏幕像素 → 纹理 uv 的步长：**逐分量相除**（写成倒数会把画面拉伸变形）
  vec2 uvStep = card / (texSize * scale);
  vec2 pixelStep = uvStep / card;

  // 4) 自由边：沿自由轴取三个样本做模糊，越靠自由边模糊越强
  vec2 axis = portrait ? vec2(0.0, 1.0) : vec2(1.0, 0.0);
  float blurAmount = smoothstep(kFreeBlurStart, 1.0, freeCoord);
  vec2 rippleUV = vec2(0.85, 0.35) * (rippleAmount * kHeatPixels) * pixelStep;
  vec2 uvCenter = (0.5 - 0.5 * uvStep) + localUV * uvStep + rippleUV;
  vec2 uvSide = axis * (pixelStep * kEdgeBlurPixels * blurAmount);
  vec3 cSharp = texture(uCover, clamp(uvCenter, 0.0, 1.0)).rgb;
  vec3 cSide = (
      texture(uCover, clamp(uvCenter - uvSide, 0.0, 1.0)).rgb +
      texture(uCover, clamp(uvCenter + uvSide, 0.0, 1.0)).rgb) * 0.5;
  vec3 cardColor = mix(cSharp, cSide, blurAmount * kEdgeBlurWeight);

  // 5) 自由边的最后一段渐隐到渐变底（贴住屏幕的三条边本来就看不到接缝）
  float fade = smoothstep(kFreeFadeStart, 1.0, freeCoord);
  color = mix(color, cardColor, inside * (1.0 - fade) * coverOn);

  // 6) 亮暗遮罩：整幅画面朝主题背景色靠一层（浅色主题提亮、深色主题压暗）
  color = mix(color, render_info.uMask.rgb, mix(kMaskDay, kMaskNight, night));

  // 7) 轻微暗角：把视线收在画面中间
  float vignette = distance(uv, vec2(0.5, 0.5));
  color *= 1.0 - 0.14 * smoothstep(0.30, 1.30, vignette);

  frag_color = vec4(max(color, vec3(0.0)), 1.0);
}
