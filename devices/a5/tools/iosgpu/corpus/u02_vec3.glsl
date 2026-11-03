// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform vec3 k;
void main() {
    gl_FragColor = vec4(k, 1.0);
}
