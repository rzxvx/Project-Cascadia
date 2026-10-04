// vertex
attribute vec4 p;
attribute vec4 a;
varying vec4 v;
void main() {
    gl_Position = p;
    v = a;
}
// fragment
precision mediump float;
varying vec4 v;
uniform vec4 u0;
void main() {
    gl_FragColor = v * u0;
}
