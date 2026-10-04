// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform float k;
void main() {
    gl_FragColor = vec4(k);
}
