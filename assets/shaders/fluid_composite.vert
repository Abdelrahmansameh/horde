#version 450 core
// Goblet Cell fluid — pass 2 of 2, vertex stage.
//
// A true fullscreen quad in clip space. Deliberately NOT the world-extent quad
// blob.vert draws: the thickness buffer this composites is a SCREEN-space
// target, written by pass one through the same view-projection the scene uses,
// so its texels already line up with pixels and there is nothing to reproject.
// Running it through the camera matrix again would double-transform it.

layout(location = 0) in vec2 a_corner; // shared unit quad, [-0.5, 0.5]

out vec2 v_uv;

void main() {
    v_uv = a_corner + 0.5;
    gl_Position = vec4(a_corner * 2.0, 0.0, 1.0);
}
