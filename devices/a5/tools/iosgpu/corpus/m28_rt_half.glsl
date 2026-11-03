// pages: all
// rt: half
// a target in RGBA half floats (EXT_color_buffer_half_float): the pixel back end's words
attribute vec4 p;
attribute vec4 a;
varying mediump vec4 c;
void main() { gl_Position = p; c = a; }
// fragment
precision mediump float;
varying vec4 c;
void main() { gl_FragColor = c; }
