// pages: all
// two textures, each its own vec2 varying
attribute vec4 p;
attribute vec2 a;
attribute vec2 b;
varying mediump vec2 t0;
varying mediump vec2 t1;
void main() { gl_Position = p; t0 = a; t1 = b; }
// fragment
precision mediump float;
uniform sampler2D s0, s1;
varying vec2 t0, t1;
void main() { gl_FragColor = texture2D(s0, t0) * texture2D(s1, t1); }
