// pages: all
// tex2d: 64 64 mip
// maxlevel: 1
// GL_TEXTURE_MAX_LEVEL_APPLE
attribute vec4 p;
attribute vec2 a;
varying mediump vec2 t;
void main() { gl_Position = p; t = a; }
// fragment
precision mediump float;
uniform sampler2D s;
varying vec2 t;
void main() { gl_FragColor = texture2D(s, t); }
