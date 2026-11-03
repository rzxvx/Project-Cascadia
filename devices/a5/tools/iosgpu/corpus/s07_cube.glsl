// pages: all
// a cube map, a vec3 varying as it is
attribute vec4 p;
attribute vec3 a;
varying mediump vec3 d;
void main() { gl_Position = p; d = a - 0.5; }
// fragment
precision mediump float;
uniform samplerCube c;
varying vec3 d;
void main() { gl_FragColor = textureCube(c, d); }
