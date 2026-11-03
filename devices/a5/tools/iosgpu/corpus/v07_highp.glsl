// vertex
attribute vec4 p;
attribute vec4 a;
varying highp vec4 v;
void main() {
    gl_Position = p;
    v = a;
}
// fragment
precision highp float;
varying highp vec4 v;
void main() {
    gl_FragColor = v;
}
