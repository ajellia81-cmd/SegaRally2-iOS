#version 450
// Menu layer (premultiplied alpha) drawn over the finished picture
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D ui;
void main() {
    color = texture(ui, uv);
}
