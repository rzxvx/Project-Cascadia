// pages: all
// depth: less
// offset: 1 2
// glPolygonOffset: the ISP's depth bias
attribute vec4 p;
attribute vec4 a;
varying mediump vec4 c;
void main() { gl_Position = p; c = a; }
// fragment
precision mediump float;
varying vec4 c;
void main() { gl_FragColor = c; }
