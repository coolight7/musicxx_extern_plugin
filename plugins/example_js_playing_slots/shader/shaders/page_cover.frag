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
// 纹理来源 `musicxx.songlist.cover` **只在歌单页面内可用**：拿不到封面时
// `uCoverInfo.w` / `uTintInfo.w` 都是 0，画面只画 `uFallback`（主题背景色）的渐变，
// 不会变黑、也不会报错。
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

// 封面占比的可用范围（插件通过 args 的 uShare 调，超出范围夹住）
const float kShareMin = 0.3;
const float kShareMax = 1.0;

// 自由边的渐隐区间（占卡片长度的比例：到 kFadeEnd 时完全是主色渐变）
const float kFadeStart = 0.52;
const float kFadeEnd = 0.97;

// 主色的饱和度提升（平均色容易发灰）与渐变暗端的比例（白昼 / 夜间）
const float kSaturation = 1.25;
const float kDeepDay = 0.62;
const float kDeepNight = 0.40;

/// 饱和度轻微提升：平均色直接铺开会偏灰，拉开一点才有"这张图的主色"的感觉
vec3 boostSaturation(vec3 color) {
  float gray = dot(color, vec3(0.299, 0.587, 0.114));
  return clamp(mix(vec3(gray), color, kSaturation), 0.0, 1.0);
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
  vec3 mainColor = mix(fallback, boostSaturation(texture(uTint, vec2(0.5, 0.5)).rgb), tintOn);
  mainColor *= mix(1.0, 0.78, night);
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

  frag_color = vec4(max(color, vec3(0.0)), 1.0);
}
