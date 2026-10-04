// vertex
attribute vec4 p;
void main() { gl_Position = p; }
// fragment
precision mediump float;
uniform vec4 a[4];
void main() {
    vec4 s = vec4(0.0);
    for (int i = 0; i < 4; i++) s += a[i];
    gl_FragColor = s;
}
