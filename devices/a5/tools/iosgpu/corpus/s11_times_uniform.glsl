// pages: all
// t06 again, every page: the texel times a uniform
attribute vec4 p;
attribute vec2 a;
varying mediump vec2 t;
void main() { gl_Position = p; t = a; }
// fragment
precision mediump float;
uniform sampler2D s;
uniform vec4 k;
varying vec2 t;
void main() { gl_FragColor = texture2D(s, t) * k; }
