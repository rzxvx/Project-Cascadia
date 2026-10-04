// vertex
attribute vec4 p;
varying vec4 t;
void main() {
    gl_Position = p;
    t = p * 0.5 + 0.5;
}
// fragment
precision mediump float;
varying vec4 t;
uniform sampler2D s;
void main() {
    gl_FragColor = texture2D(s, t.xy * 2.0);
}
