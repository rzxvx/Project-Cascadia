// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;

void main() {
    gl_FragColor = vec4(0.25, 0.5, 0.75, 1.0);
}
