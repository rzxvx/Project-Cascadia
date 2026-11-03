// vertex
attribute vec4 p;
uniform mat4 m;
void main() { gl_Position = m * p; }
// fragment
precision mediump float;

void main() {
    gl_FragColor = vec4(1.0);
}
