// pages: all
// depth: less
// query: occlusion
// an occlusion query (EXT_occlusion_query_boolean): its render command
attribute vec4 p;
attribute vec4 a;
varying mediump vec4 c;
void main() { gl_Position = p; c = a; }
// fragment
precision mediump float;
varying vec4 c;
void main() { gl_FragColor = c; }
