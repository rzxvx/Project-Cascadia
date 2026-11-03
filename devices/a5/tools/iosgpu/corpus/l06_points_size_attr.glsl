// pages: all
// mode: points
// each point its own size, from an attribute
attribute vec4 p;
attribute float a;
void main() { gl_Position = p; gl_PointSize = a * 8.0; }
// fragment
precision mediump float;
void main() { gl_FragColor = vec4(1.0, 0.5, 0.25, 1.0); }
