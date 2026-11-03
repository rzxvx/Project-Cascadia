// pages: all
// blend: alpha
// draws: 3
// s01 drawn three times in one frame, the texture another each time: how
// the state of a texture changes between draws
attribute vec4 p;
attribute vec4 a_color;
attribute vec2 a_texCoord;
varying mediump vec4 v_color;
varying mediump vec2 v_texCoord;
void main() { gl_Position = p; v_color = a_color; v_texCoord = a_texCoord; }
// fragment
uniform sampler2D u_texture;
varying mediump vec4 v_color;
varying mediump vec2 v_texCoord;
void main() { gl_FragColor = texture2D(u_texture, v_texCoord) * v_color; }
