// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;

void main() {
    gl_FragColor = gl_FragCoord * 0.015625;
}
