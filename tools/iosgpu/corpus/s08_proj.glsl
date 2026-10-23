// pages: all
// texture2DProj of a vec3 varying as it is
attribute vec4 p;
attribute vec3 a;
varying mediump vec3 t;
void main() { gl_Position = p; t = a; }
// fragment
precision mediump float;
uniform sampler2D s;
varying vec3 t;
void main() { gl_FragColor = texture2DProj(s, t); }
