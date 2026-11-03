// vertex
attribute vec4 p;

void main() {
    gl_Position = p;
    gl_PointSize = 4.0;
}
// fragment
precision mediump float;

void main() {
    gl_FragColor = vec4(1.0);
}
