#version 460 core

// 歌曲页面的页面背景示例：把当前打开的歌单封面贴在屏幕边缘占 70%，
// 余下部分渐变到这张图的主色调。
//
//   · 横屏（res.y < res.x）：封面贴左、上、下三条边，占屏幕宽的 70%，右边渐隐到主色；
//   · 竖屏（res.y >= res.x）：封面贴上、左、右三条边，占屏幕高的 70%，下边渐隐到主色。
//
// 主色调来自第二张纹理 `uTint`：同一条来源（`musicxx.songlist.cover`）声明成
// `size: 128, blur: 40, square: true` 的小图。模糊到这个程度基本是一张纯色，
// 采样一次就相当于整张封面的平均色 —— 插件拿不到像素数据，这是最省的做法。
//
// **配色跟着封面走，并且保证页面文字看得清**：
// 页面上的标题、歌曲名是主程序的文字色，夜间是浅色、白昼是深色。背景要是跟文字一个调子，
// 文字就糊在图里了。所以最后一步把整幅画面（封面与渐变一起）的亮度收进"与当前主题文字
// 有足够对比度"的区间：区间内的像素原样保留（封面的层次、主色的色相都在），只有超出区间的
// 像素被压暗或提亮。区间按对比度反推：
//   · 夜间（浅色文字）：亮度 ≤ kLumNightMax，与白色约 7:1；
//   · 白昼（深色文字）：亮度 ≥ kLumDayMin，与 #323232 约 4.8:1。
// 这段判断全在插件里做，主程序不参与，也不需要新的接口。
//
// 纹理来源 `musicxx.songlist.cover` **只在歌单页面内可用**：拿不到封面时
// `uCoverInfo.w` / `uTintInfo.w` 都是 0，画面只画 `uFallback`（主题背景色）的渐变，
// 不会变黑、也不会报错（主题背景色本来就在可读区间里，最后一步不会动它）。
//
// uniform 约定（主程序每帧填充，成员名不能改）：
//   uParams    = (目标宽, 目标高, 时间秒, 速度)
//   uEnv       = (是否夜间, 是否有封面配色, 是否有频谱数据, 保留)
//   uCoverInfo = (纹理宽, 纹理高, 原图宽高比, 是否有封面)
//   uTintInfo  = 同上（主色小图）
//   uFallback  = 没有封面时的底色（插件在 args 里给主题背景色）
//   uShare     = x 是封面占屏幕的比例（示例用 0.7）
// 纹理：uCover = 清晰封面、uTint = 模糊成近似纯色的同一张图

uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uEnv;
  vec4 uCoverInfo;
  vec4 uTintInfo;
  vec4 uFallback;
  vec4 uShare;
} render_info;

uniform sampler2D uCover;
uniform sampler2D uTint;

layout(location = 0) out vec4 frag_color;

// 亮度权重（人眼对绿色最敏感，与主程序算封面亮度用的是同一组）
const vec3 kLumWeight = vec3(0.299, 0.587, 0.114);

// 封面占比的可用范围（插件通过 args 的 uShare 调，超出范围夹住）
const float kShareMin = 0.3;
const float kShareMax = 1.0;

// 自由边的渐隐区间（占卡片长度的比例：到 kFadeEnd 时完全是主色渐变）
const float kFadeStart = 0.52;
const float kFadeEnd = 0.97;

// 主色的饱和度提升（平均色容易发灰，拉开一点才有"这张图的主色"的感觉）
const float kSaturation = 1.25;

// 渐变暗端相对主色的比例（保留一点层次，不要一整块纯色）
const float kDeepDay = 0.86;
const float kDeepNight = 0.55;

// 文字看得清要求的亮度区间（见文件头的推导）
const float kLumNightMin = 0.05;
const float kLumNightMax = 0.34;
const float kLumDayMin = 0.62;
const float kLumDayMax = 0.95;

/// 饱和度轻微提升：平均色直接铺开会偏灰，拉开一点才有"这张图的主色"的感觉
vec3 boostSaturation(vec3 color) {
  float gray = dot(color, kLumWeight);
  return clamp(mix(vec3(gray), color, kSaturation), 0.0, 1.0);
}

/// 把颜色拉到目标亮度，色相与相对饱和度尽量保留
/// * 压暗：各分量等比例缩小，不会溢出
/// * 提亮：向白色混合（按当前亮度解出比例），分量不会超过 1
vec3 setLuminance(vec3 color, float target) {
  float lum = clamp(dot(color, kLumWeight), 0.0, 1.0);
  target = clamp(target, 0.0, 1.0);
  if (target <= lum) {
    return color * (target / max(lum, 1e-3));
  }
  float t = (target - lum) / max(1.0 - lum, 1e-3);
  return mix(color, vec3(1.0), t);
}

/// 把颜色收进 [low, high] 这个亮度区间；区间内的像素原样返回
///
/// 这就是"按主题文字反推背景亮暗"那一步：亮度在区间里说明文字已经看得清，不要动它。
vec3 clampToReadable(vec3 color, float low, float high) {
  float lum = clamp(dot(color, kLumWeight), 0.0, 1.0);
  if (lum >= low && lum <= high) {
    return color;
  }
  return setLuminance(color, clamp(lum, low, high));
}

void main() {
  vec2 res = max(render_info.uParams.xy, vec2(1.0));
  // 渲染目标是左上角原点、y 向下：贴住左上角就等于"贴上边与左边"，
  // 卡片的高度（横屏）/ 宽度（竖屏）占满，自然还贴住下边（横屏）/ 右边（竖屏）
  vec2 px = gl_FragCoord.xy;

  float night = render_info.uEnv.x > 0.5 ? 1.0 : 0.0;
  float coverOn = render_info.uCoverInfo.w > 0.5 ? 1.0 : 0.0;
  float tintOn = render_info.uTintInfo.w > 0.5 ? 1.0 : 0.0;
  bool portrait = res.y >= res.x;

  // 1) 主色调：模糊过的小图取一次中心采样；没有封面时退回主题背景色
  vec3 fallback = render_info.uFallback.rgb;
  vec3 tintColor = boostSaturation(texture(uTint, vec2(0.5, 0.5)).rgb);
  vec3 mainColor = mix(fallback, tintColor, tintOn);
  vec3 deepColor = mainColor * mix(kDeepDay, kDeepNight, night);

  // 2) 卡片范围：横屏占 70% 宽、满高；竖屏满宽、占 70% 高；都贴左上角
  float share = clamp(render_info.uShare.x, kShareMin, kShareMax);
  vec2 card = portrait ? vec2(res.x, res.y * share) : vec2(res.x * share, res.y);
  card = max(card, vec2(1.0));
  float inside = (1.0 - step(card.x, px.x)) * (1.0 - step(card.y, px.y));
  // 卡片内的局部坐标与"自由轴"上的位置（0 = 贴住屏幕的边，1 = 自由边）
  vec2 local = px / card;
  float freeCoord = portrait ? local.y : local.x;

  // 3) 渐变底：从主色过渡到更暗的同色，方向沿自由轴
  float along = portrait ? (px.y / res.y) : (px.x / res.x);
  vec3 color = mix(mainColor, deepColor, smoothstep(0.0, 1.0, clamp(along, 0.0, 1.0)));

  // 4) 卡片里的封面：按 cover 铺满（采样步长逐分量相除，否则会被拉伸）
  vec2 texSize = max(render_info.uCoverInfo.xy, vec2(1.0));
  float fillScale = max(card.x / texSize.x, card.y / texSize.y);
  vec2 uvStep = card / (texSize * fillScale);
  vec2 uv = clamp((0.5 - 0.5 * uvStep) + local * uvStep, 0.0, 1.0);
  vec3 coverColor = texture(uCover, uv).rgb;

  // 5) 自由边一段渐隐到渐变底（贴住屏幕的三条边本来就看不出接缝）
  float fade = smoothstep(kFadeStart, kFadeEnd, freeCoord);
  color = mix(color, coverColor, inside * coverOn * (1.0 - fade));

  // 6) 轻微暗角：把视线收在画面靠左（横屏）/ 靠上（竖屏）的封面上
  vec2 q = (px - 0.5 * res) / max(0.5 * min(res.x, res.y), 1.0);
  color *= 1.0 - 0.10 * smoothstep(0.6, 1.6, length(q)) * (0.6 + 0.4 * night);

  // 7) 亮度收进主题文字能看清的区间：封面亮的地方夜间被压暗、封面暗的地方白昼被提亮，
  //    区间内的部分（大多数时候是渐变底与中间调）原样保留，不整体泛白 / 泛黑
  color = clampToReadable(
    color,
    mix(kLumDayMin, kLumNightMin, night),
    mix(kLumDayMax, kLumNightMax, night)
  );

  frag_color = vec4(max(color, vec3(0.0)), 1.0);
}
