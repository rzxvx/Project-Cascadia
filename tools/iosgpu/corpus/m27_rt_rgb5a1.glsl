// pages: all
// rt: rgb5a1
// a target in RGBA 5-5-5-1: the pixel back end's words
attribute vec4 p;
attribute vec4 a;
varying mediump vec4 c;
void main() { gl_Position = p; c = a; }
// fragment
precision mediump float;
varying vec4 c;
void main() { gl_FragColor = c; }
