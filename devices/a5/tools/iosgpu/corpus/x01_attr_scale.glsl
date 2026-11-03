// vertex
attribute vec4 p;
attribute vec4 a;
varying vec4 v;
void main() {
    gl_Position = p;
    v = a * 2.0;
}
// fragment
precision mediump float;
varying vec4 v;
void main() {
    gl_FragColor = v;
}
