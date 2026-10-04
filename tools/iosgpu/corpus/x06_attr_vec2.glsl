// vertex
attribute vec4 p;
attribute vec2 a;
varying vec4 v;
void main() {
    gl_Position = p;
    v = vec4(a, 0.0, 1.0);
}
// fragment
precision mediump float;
varying vec4 v;
void main() {
    gl_FragColor = v;
}
