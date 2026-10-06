#version 460 core

// 播放页背景示例着色器：把宿主每帧写入的 4 个绘制色画成缓慢流动的"晶格化"图案；
// 声明了封面（`cover`）并且当前歌曲有封面时，还在下面铺一层**预模糊的封面**。
//
// 做法（Worley / 随机点最近邻）：把画面按固定格子切开，每格放一个随时间缓慢漂移的随机点，
// 取离当前像素最近的那个点所在的位置作为采样坐标，再去混合 4 个绘制色 —— 于是画面被切成
// 一块块多边形，边界随点漂移而流动。
//
// 封面铺底（`cover` 声明里的 `blur` 由宿主预生成，着色器直接采样，不需要自己模糊）：
// 模糊封面按 cover 方式铺满画面（超出部分裁掉），再叠上晶格图案；没有封面时画面与
// 不带封面时完全一致（只看 `uCoverBackInfo.w`，不要看采样到的像素值）。
//
// uniform 约定（宿主固定填充，成员名字不能改）：
//   uParams = (目标宽, 目标高, 时间秒, 速度)
//   uEnv.x = 是否夜间；uEnv.y = 调色板是否有效（0 = 用的是备用色）
//   uColor1..uColor4 = 4 个绘制色（按插件声明的来源解析）
//   uCoverBackInfo = (纹理宽, 纹理高, 原图宽高比, 是否有封面) —— 声明了 cover 才有
// 纹理：uniform sampler2D uCoverBack（预模糊的封面，声明了 cover 才会被绑定）
uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uEnv;
  vec4 uColor1;
  vec4 uColor2;
  vec4 uColor3;
  vec4 uColor4;
  vec4 uCoverBackInfo;
}
render_info;

// 预模糊的封面（插件在 UI 项里声明 cover.texture = "uCoverBack"）
uniform sampler2D uCoverBack;

layout(location = 0) out vec4 frag_color;

// 每格的像素边长（越小格子越密；1280 宽时大约 25 格）
const float kCellSize = 50.0;

// 稳定随机向量：同样的格子坐标永远得到同样的点
vec2 random2(vec2 p) {
  p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
  return fract(sin(p) * 43758.5453);
}

// 晶格化采样坐标：返回最近随机点的所在位置（用画面比例表示）
vec2 crystallizeUV(vec2 uv) {
  vec2 cells = max(render_info.uParams.xy, vec2(1.0)) / kCellSize;
  vec2 scaled = uv * cells;
  vec2 base = floor(scaled);
  vec2 frac = fract(scaled);

  float minDist = 8.0;
  vec2 nearest = vec2(0.0);
  float timeOffset = render_info.uParams.z * render_info.uParams.w * 2.0;

  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      vec2 neighbor = vec2(float(x), float(y));
      vec2 point = random2(base + neighbor);
      // 让随机点缓慢漂移（幅度小，看起来是"流动"而不是"跳动"）
      point = 0.5 + 0.4 * sin(timeOffset + 6.2831 * point);
      vec2 diff = neighbor + point - frac;
      float dist = dot(diff, diff);
      if (dist < minDist) {
        minDist = dist;
        nearest = base + neighbor + point;
      }
    }
  }
  return nearest / cells;
}

// 4 个绘制色按"左上-右上-左下-右下"四角双线性混合
vec3 blendColors(vec2 uv, float dim) {
  float xBlend = smoothstep(0.0, 1.0, clamp(uv.x, 0.0, 1.0));
  float yBlend = smoothstep(0.0, 1.0, clamp(uv.y, 0.0, 1.0));
  vec3 top = mix(render_info.uColor1.rgb, render_info.uColor2.rgb, xBlend);
  vec3 bottom = mix(render_info.uColor3.rgb, render_info.uColor4.rgb, xBlend);
  return mix(top, bottom, yBlend) * dim;
}

void main() {
  vec2 res = max(render_info.uParams.xy, vec2(1.0));
  vec2 uv = gl_FragCoord.xy / res;
  // 稍微放大再取格子，避免边缘那一圈格子被裁成半块
  vec2 centered = (uv - 0.5) * 1.1 + 0.5;
  vec2 crystal = crystallizeUV(centered);

  // 夜间压暗；用的是备用色时降一点对比度
  float dim = render_info.uEnv.x > 0.5 ? 0.72 : 1.0;
  float valid = render_info.uEnv.y > 0.5 ? 1.0 : 0.88;
  vec3 color = blendColors(crystal, 1.0);

  // 封面铺底：把预模糊的封面按 cover 方式铺满画面，再叠上晶格图案。
  // 没有封面（还没加载好 / 这首歌没有封面）时 `uCoverBackInfo.w` 是 0，画面与原来一致。
  float coverOn = render_info.uCoverBackInfo.w > 0.5 ? 1.0 : 0.0;
  vec2 texSize = max(render_info.uCoverBackInfo.xy, vec2(1.0));
  // cover 裁切：纹理铺满画面、超出部分裁掉（不拉伸变形）
  float coverScale = max(res.x / texSize.x, res.y / texSize.y);
  vec2 coverRatio = (texSize * coverScale) / res;
  vec2 coverUV = (uv - 0.5) * coverRatio + 0.5;
  vec3 coverColor = texture(uCoverBack, coverUV).rgb;
  color = mix(color, coverColor, coverOn * 0.58);

  // 封面版整体压暗一点：模糊的亮封面铺满后，前景浅色文字会看不清
  color *= dim * valid * mix(1.0, 0.86, coverOn);

  // 格子边缘加一层很淡的高光，让晶格边界更清楚
  vec2 edgeDist = abs(centered - crystal) * 8.0;
  float edge = 1.0 - clamp(max(edgeDist.x, edgeDist.y), 0.0, 1.0);
  color += color * edge * 0.12;

  // 轻微暗角
  color *= 1.0 - 0.18 * length(uv - 0.5);
  frag_color = vec4(color, 1.0);
}
