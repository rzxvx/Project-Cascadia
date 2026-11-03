// pages: all
// tex2d: 64 64 mip linear
// aniso: 4
// EXT_texture_filter_anisotropic
attribute vec4 p;
attribute vec2 a;
varying mediump vec2 t;
void main() { gl_Position = p; t = a; }
// fragment
precision mediump float;
uniform sampler2D s;
varying vec2 t;
void main() { gl_FragColor = texture2D(s, t); }
