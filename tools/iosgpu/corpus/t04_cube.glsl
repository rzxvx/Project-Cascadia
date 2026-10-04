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
uniform samplerCube c;
void main() {
    gl_FragColor = textureCube(c, t.xyz - 0.5);
}
