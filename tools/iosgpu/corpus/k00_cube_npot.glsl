// pages: all
// cube: 15 linear
// a 15 x 15 cube map, linear (type 7, the arbitrary cube map?)
attribute vec4 p;
attribute vec3 a;
varying mediump vec3 d;
void main() { gl_Position = p; d = a - 0.5; }
// fragment
precision mediump float;
uniform samplerCube c;
varying vec3 d;
void main() { gl_FragColor = textureCube(c, d * 1.01); }
