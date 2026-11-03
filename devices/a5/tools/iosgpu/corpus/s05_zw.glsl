// pages: all
// the coordinates a vec4 varying's z and w
attribute vec4 p;
attribute vec4 a;
varying mediump vec4 t;
void main() { gl_Position = p; t = a; }
// fragment
precision mediump float;
uniform sampler2D s;
varying vec4 t;
void main() { gl_FragColor = texture2D(s, t.zw) + t.x; }
