// vertex
attribute vec4 p;
attribute vec3 n;
uniform mat3 m;
varying vec4 v;
void main() {
    gl_Position = p;
    v = vec4(m * n, 1.0);
}
// fragment
precision mediump float;
varying vec4 v;
void main() {
    gl_FragColor = v;
}
