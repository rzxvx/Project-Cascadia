// pages: all
// blend: alpha
// lowp: the texel times a lowp colour (U8 arithmetic?)
attribute vec4 p;
attribute vec4 a_color;
attribute vec2 a_texCoord;
varying lowp vec4 v_color;
varying mediump vec2 v_texCoord;
void main() { gl_Position = p; v_color = a_color; v_texCoord = a_texCoord; }
// fragment
uniform sampler2D u_texture;
varying lowp vec4 v_color;
varying mediump vec2 v_texCoord;
void main() { gl_FragColor = texture2D(u_texture, v_texCoord) * v_color; }
