// pages: all
// tex2d: 64 64 mip
// texture2DLodEXT (EXT_shader_texture_lod) in a fragment shader
attribute vec4 p;
attribute vec2 a;
varying mediump vec2 t;
void main() { gl_Position = p; t = a; }
// fragment
#extension GL_EXT_shader_texture_lod : require
precision mediump float;
uniform sampler2D s;
varying vec2 t;
void main() { gl_FragColor = texture2DLodEXT(s, t, 1.0); }
