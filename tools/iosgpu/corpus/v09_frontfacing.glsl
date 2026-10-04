// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;

void main() {
    gl_FragColor = gl_FrontFacing ? vec4(1.0) : vec4(0.5);
}
