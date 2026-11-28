#version 460 core

// 播放页歌曲图内容『热浪』：把当前歌曲图当作纹理，叠一层向上升起的热气流扰动 + 轻微色散，
// 看起来像"隔着被晒热的空气看图片"。
//
// 插件的 shader 块里声明纹理 `uCover`（`musicxx.cover`，方形裁剪）与参数 `uStrength`：
// 主程序解码当前歌曲图、上传成纹理，着色器只做扰动与色散，不搬字节。
//
// uniform 约定（主程序每帧填充，成员名不能改）：
//   uParams    = (目标宽, 目标高, 时间秒, 速度)
//   uCoverInfo = (纹理宽, 纹理高, 原图宽高比, 是否有封面)
//   uStrength  = x 是波动幅度倍率（1 = 基准，0 = 不动；插件在 args 里声明）
// 纹理：uniform sampler2D uCover（当前歌曲图；没有封面时是 1×1 透明占位）
uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uCoverInfo;
  vec4 uStrength;
} render_info;

uniform sampler2D uCover;

layout(location = 0) out vec4 frag_color;

// 两层行波的疏密与推进速度（按画面 uv 数波形，不随分辨率变化）
const float kWaveFreqA = 22.0;
const float kWaveFreqB = 37.0;
const float kWaveSpeedA = 3.4;
const float kWaveSpeedB = 5.1;

// 位移上限（占画面的比例）：再大就认不出图片了
const float kMaxShift = 0.012;

// 红蓝通道的错开量（色散）：热空气折射下边缘会有彩色抖动
const float kFringe = 0.0035;

void main() {
  vec2 res = max(render_info.uParams.xy, vec2(1.0));
  float t = render_info.uParams.z * max(render_info.uParams.w, 0.0001);
  vec2 uv = gl_FragCoord.xy / res;
  float strength = max(render_info.uStrength.x, 0.0);
  // 渲染目标的 y 向下（uv.y = 0 是图片顶部）：热浪从底部升起，越靠下越强
  float rise = 0.35 + 0.65 * uv.y;

  // 两层行波叠加：低频那层决定整体摆动，高频那层做细密抖动
  float waveA = sin(
      uv.y * kWaveFreqA - t * kWaveSpeedA + sin(uv.x * 6.0 + t * 0.9) * 1.8);
  float waveB = sin(uv.y * kWaveFreqB + t * kWaveSpeedB + uv.x * 3.5);
  vec2 shift = vec2(waveA + 0.5 * waveB, 0.35 * waveB) *
      (kMaxShift * strength * rise);

  // 铺满（cover）：屏幕坐标 → 纹理坐标，**逐分量相除**，横竖比不同也不会拉伸变形
  vec2 texSize = max(render_info.uCoverInfo.xy, vec2(1.0));
  float fill = max(res.x / texSize.x, res.y / texSize.y);
  vec2 uvStep = res / (texSize * fill);
  vec2 baseUV = (uv - 0.5) * uvStep + 0.5 + shift;

  vec3 color = texture(uCover, clamp(baseUV, 0.0, 1.0)).rgb;
  // 色散：红蓝通道反向偏一点
  float r = texture(
      uCover, clamp(baseUV + vec2(kFringe * strength, 0.0), 0.0, 1.0)).r;
  float b = texture(
      uCover, clamp(baseUV - vec2(kFringe * strength, 0.0), 0.0, 1.0)).b;
  color = vec3(r, color.g, b);
  // 亮度随波纹轻微起伏
  color *= 1.0 + 0.05 * strength * sin(uv.y * 14.0 - t * 2.6);

  // 没有封面：输出透明，让应用画的默认图 / 回退图透出来（不要靠采样到的颜色猜）
  float coverOn = render_info.uCoverInfo.w > 0.5 ? 1.0 : 0.0;
  frag_color = vec4(max(color, vec3(0.0)), coverOn);
}
