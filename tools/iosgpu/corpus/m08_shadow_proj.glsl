// pages: all
// texfmt: depth16
// compare
// shadow2DProjEXT
attribute vec4 p;
attribute vec2 a;
varying mediump vec3 t;
void main() { gl_Position = p; t = vec3(a, 0.5); }
// fragment
#extension GL_EXT_shadow_samplers : require
precision mediump float;
uniform sampler2DShadow s;
varying vec3 t;
void main() { gl_FragColor = vec4(shadow2DProjEXT(s, vec4(t, 2.0))); }
