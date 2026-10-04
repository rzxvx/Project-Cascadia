// vertex
attribute vec4 p;
attribute vec4 a;
varying vec4 v0, v1, v2, v3, v4, v5, v6, v7;
void main() {
    gl_Position = p;
    v0 = a; v1 = a * 2.0; v2 = a * 3.0; v3 = a * 4.0;
    v4 = a * 5.0; v5 = a * 6.0; v6 = a * 7.0; v7 = a * 8.0;
}
// fragment
precision mediump float;
varying vec4 v0, v1, v2, v3, v4, v5, v6, v7;
void main() {
    gl_FragColor = v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7;
}
