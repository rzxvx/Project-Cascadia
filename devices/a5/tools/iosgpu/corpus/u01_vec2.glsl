// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform vec2 k;
void main() {
    gl_FragColor = vec4(k, 0.0, 1.0);
}
