#version 460 core

// 播放页背景示例着色器：用宿主每帧写入的 4 个绘制色画一层缓慢流动的渐变。
//
// uniform 契约（宿主固定填充，成员名字不能改）：
//   uParams = (目标宽, 目标高, 时间秒, 速度)
//   uEnv.x = 是否夜间；uEnv.y = 调色板是否有效（0 = 用的是兜底色）
//   uColor1..uColor4 = 4 个绘制色（按插件声明的来源解析）
uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uEnv;
  vec4 uColor1;
  vec4 uColor2;
  vec4 uColor3;
  vec4 uColor4;
}
render_info;

layout(location = 0) out vec4 frag_color;

// 二维旋转
vec2 rotate(vec2 p, float a) {
  float s = sin(a);
  float c = cos(a);
  return vec2(p.x * c - p.y * s, p.x * s + p.y * c);
}

void main() {
  // 以 5 秒为周期的时间（速度由插件声明的 speed 决定）
  float t = render_info.uParams.z * render_info.uParams.w * 0.2;
  vec2 uv = gl_FragCoord.xy / max(render_info.uParams.xy, vec2(1.0));

  // 四个角分别用一个绘制色，做双线性混合
  vec4 c0 = render_info.uColor1;
  vec4 c1 = render_info.uColor2;
  vec4 c2 = render_info.uColor3;
  vec4 c3 = render_info.uColor4;

  vec2 warped = rotate(uv - 0.5, t * 0.15) + 0.5;
  warped += 0.06 * vec2(sin(t + uv.y * 3.0), cos(t * 0.8 + uv.x * 3.0));

  vec4 bottom = mix(c0, c1, clamp(warped.x, 0.0, 1.0));
  vec4 top = mix(c3, c2, clamp(warped.x, 0.0, 1.0));
  vec4 color = mix(bottom, top, clamp(warped.y, 0.0, 1.0));

  // 夜间压暗一点，兜底色时降低对比度（模拟有效调色板的效果）
  float dim = render_info.uEnv.x > 0.5 ? 0.72 : 1.0;
  float valid = render_info.uEnv.y > 0.5 ? 1.0 : 0.85;
  color.rgb *= dim * valid;

  // 轻微暗角，避免纯色块太生硬
  float vignette = 1.0 - 0.25 * length(uv - 0.5);
  frag_color = vec4(color.rgb * vignette, 1.0);
}
