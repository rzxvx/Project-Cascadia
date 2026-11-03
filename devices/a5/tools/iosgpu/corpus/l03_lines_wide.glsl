// pages: all
// mode: lines
// width: 4
// lines 4 pixels wide
attribute vec4 p;
attribute vec4 a;
varying mediump vec4 c;
void main() { gl_Position = p; c = a; }
// fragment
precision mediump float;
varying vec4 c;
void main() { gl_FragColor = c; }
