// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform vec4 k0, k1, k2, k3, k4, k5, k6, k7;
void main() {
    gl_FragColor = k0 + k1 + k2 + k3 + k4 + k5 + k6 + k7;
}
