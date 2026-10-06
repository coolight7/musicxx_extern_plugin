#version 460 core

// 全屏三角形：宿主绑定 3 个顶点（覆盖整个裁剪空间），不做任何变换。
layout(location = 0) in vec2 position;

void main() { gl_Position = vec4(position, 0.0, 1.0); }
