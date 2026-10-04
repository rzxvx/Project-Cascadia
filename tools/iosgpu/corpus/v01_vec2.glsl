// vertex
attribute vec4 p;
attribute vec4 a;
varying vec2 v;
void main() {
    gl_Position = p;
    v = a.xy;
}
// fragment
precision mediump float;
varying vec2 v;
void main() {
    gl_FragColor = vec4(v, 0.0, 1.0);
}
