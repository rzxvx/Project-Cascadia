// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform mat2 m;
uniform vec4 u0;
void main() {
    gl_FragColor = vec4(m * u0.xy, 0.0, 1.0);
}
