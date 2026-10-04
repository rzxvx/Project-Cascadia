// vertex
attribute vec4 p;
attribute vec4 a, b;
varying vec4 v;
varying vec2 w;
void main() {
    gl_Position = p;
    v = a;
    w = b.xy;
}
// fragment
precision mediump float;
varying vec4 v;
varying vec2 w;
void main() {
    gl_FragColor = v + vec4(w, 0.0, 0.0);
}
