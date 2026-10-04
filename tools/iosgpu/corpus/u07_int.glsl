// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform int i;
void main() {
    gl_FragColor = vec4(float(i));
}
