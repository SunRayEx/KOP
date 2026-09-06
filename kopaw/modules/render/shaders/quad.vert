#version 450
// 全屏四边形（三角带 4 顶点，无顶点缓冲）；视频图像保持正立方向。
layout(location = 0) out vec2 uv;

void main() {
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    uv = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
