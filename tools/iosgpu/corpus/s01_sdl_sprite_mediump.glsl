// pages: all
// blend: alpha
// as s00, mediump texture coordinates (SDL's other precision)
attribute vec4 p;
attribute vec4 a_color;
attribute vec2 a_texCoord;
varying mediump vec4 v_color;
varying mediump vec2 v_texCoord;
void main() {
    v_texCoord = a_texCoord;
    gl_Position = p;
    v_color = a_color;
}
// fragment
uniform sampler2D u_texture;
varying mediump vec4 v_color;
varying mediump vec2 v_texCoord;
void main() {
    gl_FragColor = texture2D(u_texture, v_texCoord) * v_color;
}
