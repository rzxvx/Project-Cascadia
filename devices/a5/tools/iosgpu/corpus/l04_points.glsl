// pages: all
// mode: points
// points 4 pixels across
attribute vec4 p;
attribute vec4 a;
varying mediump vec4 c;
void main() { gl_Position = p; gl_PointSize = 4.0; c = a; }
// fragment
precision mediump float;
varying vec4 c;
void main() { gl_FragColor = c; }
