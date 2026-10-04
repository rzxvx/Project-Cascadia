// vertex
attribute vec4 p;
attribute vec4 a;
varying lowp vec4 v;
void main() {
    gl_Position = p;
    v = a;
}
// fragment
precision mediump float;
varying lowp vec4 v;
void main() {
    gl_FragColor = v;
}
