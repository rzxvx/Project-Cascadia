// pages: all
// depth: less
// discard with depth: the punch-through pass -- its render command
attribute vec4 p;
attribute vec4 a;
varying mediump vec4 c;
void main() { gl_Position = p; c = a; }
// fragment
precision mediump float;
varying vec4 c;
void main() { if (c.x < 0.3) discard; gl_FragColor = c; }
