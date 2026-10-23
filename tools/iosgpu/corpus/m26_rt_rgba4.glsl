// pages: all
// rt: rgba4
// a target in RGBA 4-4-4-4: the pixel back end's words
attribute vec4 p;
attribute vec4 a;
varying mediump vec4 c;
void main() { gl_Position = p; c = a; }
// fragment
precision mediump float;
varying vec4 c;
void main() { gl_FragColor = c; }
