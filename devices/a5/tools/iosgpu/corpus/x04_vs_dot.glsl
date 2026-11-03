// vertex
attribute vec4 p;
attribute vec4 a;
uniform vec4 k;
varying vec4 v;
void main() {
    gl_Position = p;
    v = vec4(dot(a, k));
}
// fragment
precision mediump float;
varying vec4 v;
void main() {
    gl_FragColor = v;
}
