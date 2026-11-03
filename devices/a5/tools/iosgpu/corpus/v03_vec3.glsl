// vertex
attribute vec4 p;
attribute vec4 a;
varying vec3 v;
void main() {
    gl_Position = p;
    v = a.xyz;
}
// fragment
precision mediump float;
varying vec3 v;
void main() {
    gl_FragColor = vec4(v, 1.0);
}
