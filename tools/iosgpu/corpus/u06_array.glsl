// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform vec4 a[4];
void main() {
    gl_FragColor = a[1] + a[3];
}
