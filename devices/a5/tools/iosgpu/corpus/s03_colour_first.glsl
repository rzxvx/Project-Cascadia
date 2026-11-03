// pages: all
// the colour varying declared and written first, then the coordinates
attribute vec4 p;
attribute vec4 a;
attribute vec2 b;
varying mediump vec4 c;
varying mediump vec2 t;
void main() { gl_Position = p; c = a; t = b; }
// fragment
precision mediump float;
uniform sampler2D s;
varying vec4 c;
varying vec2 t;
void main() { gl_FragColor = c * texture2D(s, t); }
