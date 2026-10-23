// pages: all
// blend: alpha
// SDL 3's GLES 2 renderer's textured shader (highp texture coordinates), the
// position "p" (the corpus's triangle) -- the PDS's texture fetch (M34)
attribute vec4 p;
attribute vec4 a_color;
attribute vec2 a_texCoord;
varying mediump vec4 v_color;
varying highp vec2 v_texCoord;
void main() {
    v_texCoord = a_texCoord;
    gl_Position = p;
    gl_PointSize = 1.0;
    v_color = a_color;
}
// fragment
uniform sampler2D u_texture;
varying mediump vec4 v_color;
varying highp vec2 v_texCoord;
void main() {
    gl_FragColor = texture2D(u_texture, v_texCoord);
    gl_FragColor *= v_color;
}
