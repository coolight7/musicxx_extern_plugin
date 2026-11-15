#version 460 core

// 播放页音频动效示例：4 根跟着低频带起伏的柱子 + 底部呼吸光晕。
//
// 这是**动效槽位**（`musicxx.ui.playing.visualizer`）的着色器：它画在播放页的动效区上，
// 下面还有页面背景，所以画面是透明的（预乘 alpha），只画柱子和光晕，不铺底。
//
// uniform 约定（主程序每帧填充，成员名不能改）：
//   uParams  = (目标宽, 目标高, 时间秒, 速度)
//   uEnv     = (是否夜间, 是否有封面配色, 是否有频谱数据, 保留)
//   uColor1 / uColor2 = 封面配色映射出来的绘制色（插件在 args 里声明）
//   uLevel   = 当前响度（x = y = z = w，0~1）
//   uBands0  = 最低的 4 个频带（0~1，低频在前）
//
// 没有频谱数据时（`uEnv.z` = 0）柱子按固定的小高度轻轻摆（时间驱动），不要靠数值 0 判断
// "没数据"：静音时频带也是 0。

uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uEnv;
  vec4 uColor1;
  vec4 uColor2;
  vec4 uLevel;
  vec4 uBands0;
} render_info;

layout(location = 0) out vec4 frag_color;

// 柱子的左右留白（占可用宽度的比例）、柱宽占每格的比例
const float kSideMargin = 0.18;
const float kBarWidth = 0.48;
// 底部留白与最高高度（占画面高的比例）
const float kBottomInset = 0.12;
const float kBarsMaxHeight = 0.5;

void main() {
  vec2 res = max(render_info.uParams.xy, vec2(1.0));
  float t = render_info.uParams.z * max(render_info.uParams.w, 0.0001);
  float night = render_info.uEnv.x > 0.5 ? 1.0 : 0.0;
  float hasSpectrum = render_info.uEnv.z > 0.5 ? 1.0 : 0.0;
  float level = hasSpectrum * clamp(render_info.uLevel.x, 0.0, 1.0);

  // 4 根柱子：按 x 分格（一个格一根），每格取一个频带
  float margin = kSideMargin * res.x;
  float usable = max(res.x - margin * 2.0, 1.0);
  float cell = usable / 4.0;
  float x = gl_FragCoord.x - margin;
  float inRange = step(0.0, x) * step(x, usable);
  float idx = floor(clamp(x, 0.0, usable - 0.001) / cell);
  float band = idx < 0.5 ? render_info.uBands0.x
      : (idx < 1.5 ? render_info.uBands0.y
      : (idx < 2.5 ? render_info.uBands0.z : render_info.uBands0.w));

  // 没有频谱时给一个随时间和柱序缓慢摆动的固定高度
  float idle = 0.26 + 0.05 * sin(t * 1.4 + idx * 1.7);
  float amp = mix(idle, max(band, 0.04), hasSpectrum);

  float bottom = kBottomInset * res.y;
  float top = min(bottom + (0.08 + kBarsMaxHeight * amp) * res.y, res.y * 0.96);
  float halfWidth = cell * kBarWidth * 0.5;
  float centerX = margin + (idx + 0.5) * cell;

  // 柱子的距离场（竖直矩形；边缘 1 像素过渡，避免硬边）
  float dx = abs(gl_FragCoord.x - centerX) - halfWidth;
  float dy = max(bottom - gl_FragCoord.y, gl_FragCoord.y - top);
  float barMask = (1.0 - smoothstep(-1.0, 1.0, max(dx, dy))) * inRange;

  // 底部光晕：响度越大越亮、越宽
  vec2 glowCenter = vec2(res.x * 0.5, bottom);
  vec2 glowDist = (gl_FragCoord.xy - glowCenter) /
      vec2(res.x * (0.34 + 0.12 * level), res.y * 0.22);
  float glow = pow(1.0 - clamp(length(glowDist), 0.0, 1.0), 2.2) *
      (0.18 + 0.42 * level);

  vec3 barColor = mix(render_info.uColor1.rgb, render_info.uColor2.rgb,
                      clamp(top / res.y, 0.0, 1.0));
  barColor *= mix(1.0, 0.88, night);
  vec3 color = barColor * barMask + render_info.uColor2.rgb * glow;
  float alpha = clamp(barMask * 0.92 + glow * 0.85, 0.0, 1.0);
  // 预乘 alpha：透明处是 0，页面背景透得出来
  frag_color = vec4(color * alpha, alpha);
}
