// vertex
attribute vec4 p;
attribute vec4 a;
varying float v;
void main() {
    gl_Position = p;
    v = a.x;
}
// fragment
precision mediump float;
varying float v;
void main() {
    gl_FragColor = vec4(v);
}
