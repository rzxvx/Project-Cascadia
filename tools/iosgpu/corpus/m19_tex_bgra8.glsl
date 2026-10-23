// pages: all
// texfmt: bgra8
// a texture in BGRA (APPLE_texture_format_BGRA8888): its texture words
attribute vec4 p;
attribute vec2 a;
varying mediump vec2 t;
void main() { gl_Position = p; t = a; }
// fragment
precision mediump float;
uniform sampler2D s;
varying vec2 t;
void main() { gl_FragColor = texture2D(s, t); }
