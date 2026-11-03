// pages: all
// a vec2 varying, the texel as it is
attribute vec4 p;
attribute vec2 a;
varying mediump vec2 t;
void main() { gl_Position = p; t = a; }
// fragment
precision mediump float;
uniform sampler2D s;
varying vec2 t;
void main() { gl_FragColor = texture2D(s, t); }
