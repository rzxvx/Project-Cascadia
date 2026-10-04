// vertex
attribute vec4 p;
attribute vec2 a;
attribute vec4 c;
varying mediump vec2 t;
varying lowp vec4 v;
void main() {
    gl_Position = p;
    t = a;
    v = c;
}
// fragment
precision mediump float;
varying vec2 t;
varying lowp vec4 v;
uniform sampler2D s;
void main() {
    gl_FragColor = texture2D(s, t) * v;
}
