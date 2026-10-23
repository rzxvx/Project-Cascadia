// pages: all
// depth: less
// draws: 2
// two opaque draws, depth tested
attribute vec4 p;
attribute vec4 a;
varying mediump vec4 c;
void main() { gl_Position = p; c = a; }
// fragment
precision mediump float;
varying vec4 c;
void main() { gl_FragColor = c; }
