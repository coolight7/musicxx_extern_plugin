#version 460 core

// 播放页歌曲图内容『油画』：对四个方向各取一小块颜色，算均值与方差，选最平滑的那一块当结果
// （Kuwahara 式滤镜），再把颜色压成少数几档、稍微提彩色，看起来像一层层颜料块。
//
// 采样量：4 个方向 × 2×2 = 每像素 16 次纹理取样。歌曲图盒子只有几百像素见方，
// 这个量在手机上也够用；想更"糊"可以调大画笔半径（改这里，不用改插件声明）。
//
// uniform 约定（主程序每帧填充，成员名不能改）：
//   uParams    = (目标宽, 目标高, 时间秒, 速度)
//   uCoverInfo = (纹理宽, 纹理高, 原图宽高比, 是否有封面)
// 纹理：uniform sampler2D uCover（当前歌曲图；没有封面时是 1×1 透明占位）
uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uCoverInfo;
} render_info;

uniform sampler2D uCover;

layout(location = 0) out vec4 frag_color;

// 画笔半径：短边 / 这个数（图标盒变大时笔触跟着变大，视觉比例保持不变）
const float kBrushDivisor = 70.0;

// 颜色压成几档（越大越接近原图，越小越像色块）
const float kColorLevels = 9.0;

// 颜色提鲜的倍率（1 = 不变，> 1 更浓）
const float kSaturation = 1.35;

void main() {
  vec2 res = max(render_info.uParams.xy, vec2(1.0));
  vec2 uv = gl_FragCoord.xy / res;

  // 铺满（cover）：屏幕坐标 → 纹理坐标（逐分量相除，横竖比不同也不变形）
  vec2 texSize = max(render_info.uCoverInfo.xy, vec2(1.0));
  float fill = max(res.x / texSize.x, res.y / texSize.y);
  vec2 uvStep = res / (texSize * fill);
  vec2 tuv = (uv - 0.5) * uvStep + 0.5;

  // 一个屏幕像素在纹理 uv 上的步长；画笔半径按屏幕像素算
  vec2 pxStep = uvStep / res;
  float radius = max(1.0, floor(min(res.x, res.y) / kBrushDivisor));

  // 四个方向各取 2×2 个样本：均值最"平"的那一块就是这一笔的颜色
  vec3 best = vec3(0.0);
  float bestScore = 1e9;
  for (int q = 0; q < 4; ++q) {
    vec2 dir = vec2(
        (q == 0 || q == 3) ? -1.0 : 1.0,
        (q < 2) ? -1.0 : 1.0);
    vec3 sum = vec3(0.0);
    vec3 sumSq = vec3(0.0);
    for (int sy = 0; sy < 2; ++sy) {
      for (int sx = 0; sx < 2; ++sx) {
        vec2 off = vec2(float(sx) + 0.5, float(sy) + 0.5) * dir * pxStep * radius;
        vec3 c = texture(uCover, clamp(tuv + off, 0.0, 1.0)).rgb;
        sum += c;
        sumSq += c * c;
      }
    }
    vec3 mean = sum * 0.25;
    vec3 variance = max(sumSq * 0.25 - mean * mean, vec3(0.0));
    float score = variance.r + variance.g + variance.b;
    if (score < bestScore) {
      bestScore = score;
      best = mean;
    }
  }

  // 颜料块：提彩色 + 压色阶
  float luma = dot(best, vec3(0.299, 0.587, 0.114));
  vec3 color = mix(vec3(luma), best, kSaturation);
  color = floor(color * kColorLevels + 0.5) / kColorLevels;
  // 画布纹理：很淡的斜向细纹，只在大片同色区域看得出来
  color += sin(uv.x * 41.0 + uv.y * 37.0) * 0.012;

  // 没有封面：输出透明，让应用画的默认图 / 回退图透出来
  float coverOn = render_info.uCoverInfo.w > 0.5 ? 1.0 : 0.0;
  frag_color = vec4(max(color, vec3(0.0)), coverOn);
}
