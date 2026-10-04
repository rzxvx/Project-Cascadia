// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform vec4 u0, u1, u2;
void main() {
    if (u0.x > 1.5) gl_FragColor = u1; else gl_FragColor = u2;
}
