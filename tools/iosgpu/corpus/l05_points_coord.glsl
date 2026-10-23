// pages: all
// mode: points
// points 16 across, gl_PointCoord read
attribute vec4 p;
void main() { gl_Position = p; gl_PointSize = 16.0; }
// fragment
precision mediump float;
void main() { gl_FragColor = vec4(gl_PointCoord, 0.5, 1.0); }
