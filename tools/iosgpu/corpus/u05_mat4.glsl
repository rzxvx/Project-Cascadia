// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform mat4 m;
uniform vec4 u0;
void main() {
    gl_FragColor = m * u0;
}
