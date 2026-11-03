// pages: all
// tex2d: 64 64 mip linear
// a 64 x 64 with every level, trilinear: the state words, the levels
attribute vec4 p;
attribute vec2 a;
varying mediump vec2 t;
void main() { gl_Position = p; t = a * 3.0; }
// fragment
precision mediump float;
uniform sampler2D s;
varying vec2 t;
void main() { gl_FragColor = texture2D(s, t * 1.01); }
