// pages: all
// depth: less
// gl_FragDepthEXT (EXT_frag_depth), if this iOS has it
attribute vec4 p;
attribute vec4 a;
varying mediump vec4 c;
void main() { gl_Position = p; c = a; }
// fragment
#extension GL_EXT_frag_depth : require
precision mediump float;
varying vec4 c;
void main() { gl_FragColor = c; gl_FragDepthEXT = c.y; }
