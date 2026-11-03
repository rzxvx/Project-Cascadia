// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform bool b;
void main() {
    gl_FragColor = b ? vec4(1.0) : vec4(0.0);
}
