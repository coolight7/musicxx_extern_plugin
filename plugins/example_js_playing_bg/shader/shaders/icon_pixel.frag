#version 460 core

// 播放页歌曲图内容『像素』：按短边把画面切成小方块，每块只取一次颜色并压到几档色阶，
// 读起来像老掌机的像素画。
//
// 纹理声明成 `smooth: false`（最近邻取样）时块边缘是硬边的；用线性过滤会糊成马赛克过渡。
//
// uniform 约定（主程序每帧填充，成员名不能改）：
//   uParams    = (目标宽, 目标高, 时间秒, 速度)
//   uCoverInfo = (纹理宽, 纹理高, 原图宽高比, 是否有封面)
//   uPixel     = x 是短边切多少块（4~128，插件在 args 里声明；越小越"像素"）
// 纹理：uniform sampler2D uCover（当前歌曲图；没有封面时是 1×1 透明占位）
uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uCoverInfo;
  vec4 uPixel;
} render_info;

uniform sampler2D uCover;

layout(location = 0) out vec4 frag_color;

/// 每个通道压成几档颜色（越小越像老掌机）
const float kColorLevels = 6.0;

/// 画面越大块越大的上限：块再大就只剩几个色块了
const float kMaxBlockPixels = 32.0;

void main() {
  vec2 res = max(render_info.uParams.xy, vec2(1.0));
  float blocks = clamp(render_info.uPixel.x, 4.0, 128.0);
  // 块边长（屏幕像素）：按短边切，别让横竖比把方块切成拉长的矩形
  float block = clamp(floor(min(res.x, res.y) / blocks), 2.0, kMaxBlockPixels);
  // 用块中心取样：块与块之间不会出现半像素偏移
  vec2 snapped = (floor(gl_FragCoord.xy / block) + 0.5) * block;
  vec2 uv = snapped / res;

  // 铺满（cover）：屏幕坐标 → 纹理坐标（逐分量相除，横竖比不同也不变形）
  vec2 texSize = max(render_info.uCoverInfo.xy, vec2(1.0));
  float fill = max(res.x / texSize.x, res.y / texSize.y);
  vec2 uvStep = res / (texSize * fill);
  vec2 tuv = (uv - 0.5) * uvStep + 0.5;

  vec3 color = texture(uCover, clamp(tuv, 0.0, 1.0)).rgb;
  // 像素风调色板：每个通道压到几档
  color = floor(color * kColorLevels + 0.5) / kColorLevels;
  // 很轻的颜色偏移，像老屏幕的偏色
  color = mix(color, color.brg, 0.06);

  // 没有封面：输出透明，让应用画的默认图 / 回退图透出来
  float coverOn = render_info.uCoverInfo.w > 0.5 ? 1.0 : 0.0;
  frag_color = vec4(max(color, vec3(0.0)), coverOn);
}
