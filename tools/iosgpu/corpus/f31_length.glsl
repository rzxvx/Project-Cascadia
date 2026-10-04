// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform vec4 u0;
void main() {
    gl_FragColor = vec4(length(u0.xyz));
}
