// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform vec4 u0, u1, u2;
void main() {
    gl_FragColor = vec4(reflect(u0.xyz, u1.xyz), 1.0);
}
