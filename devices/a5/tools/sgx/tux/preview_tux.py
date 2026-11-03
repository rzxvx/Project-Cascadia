# preview_tux.py -- what the real-time renderer will draw, rendered by
# Blender from the scene's camera, to compare with the Cycles render:
#
#   Blender -b build/tux/tux-rt.blend --python tools/sgx/tux/preview_tux.py   (TUX_PREVIEW=baked|lightmap)
#   Blender -b build/tux/tux-src.blend --python tools/sgx/tux/preview_tux.py  (TUX_PREVIEW=original)
#
# baked: each lit object its tux-baked.png on the lightmap UVs, unlit, as it is
#   (display-referred already: the view transform Standard here); the logo white.
# lightmap: albedo x lightmap_scale x pow(tux-lightmap.png, 2.2), the logo
#   emissive x 5, through AgX as the render.
# original: the artist's file in Cycles, fewer samples, denoised.
import json, os

import bpy

OUT = os.path.dirname(os.path.abspath(bpy.data.filepath))
MODE = os.environ.get("TUX_PREVIEW", "baked")
scene = bpy.context.scene
scene.render.resolution_x, scene.render.resolution_y = 768, 1024
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = "PNG"
scene.render.filepath = os.path.join(OUT, "preview-%s.png" % MODE)
LOGO = ("Sphere.009", "Sphere.010")

if MODE == "original":
    prefs = bpy.context.preferences.addons["cycles"].preferences
    prefs.compute_device_type = "METAL"
    prefs.get_devices()
    for d in prefs.devices:
        d.use = True
    scene.cycles.device = "GPU"
    scene.cycles.samples = 256
    scene.cycles.use_denoising = True
    bpy.ops.render.render(write_still=True)
    raise SystemExit


def emission_material(name, build):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    em = nt.nodes.new("ShaderNodeEmission")
    nt.links.new(em.outputs[0], out.inputs["Surface"])
    build(nt, em)
    return m


def uv(nt, name):
    n = nt.nodes.new("ShaderNodeUVMap")
    n.uv_map = name
    return n.outputs[0]


baked = bpy.data.images.load(os.path.join(OUT, "tux-baked.png"))
baked.colorspace_settings.name = "sRGB"
lm = bpy.data.images.load(os.path.join(OUT, "tux-lightmap.png"))
lm.colorspace_settings.name = "Non-Color"
scale = json.load(open(os.path.join(OUT, "bake.json")))["lightmap_scale"]

scene.render.engine = "BLENDER_EEVEE"
scene.world.node_tree.nodes["Background"].inputs[0].default_value = (0, 0, 0, 1)
scene.view_settings.view_transform = "Standard" if MODE == "baked" else "AgX"

for ob in scene.objects:
    if ob.type != "MESH" or not ob.material_slots:
        continue
    if ob.name in LOGO:
        strength = 1.0 if MODE == "baked" else 5.0
        m = emission_material("pv_logo", lambda nt, em: (
            em.inputs[0].__setattr__("default_value", (1, 1, 1, 1)),
            em.inputs[1].__setattr__("default_value", strength)))
    elif "lightmap" in ob.data.uv_layers:
        src = ob.material_slots[0].material

        def build(nt, em, src=src):
            if MODE == "baked":
                t = nt.nodes.new("ShaderNodeTexImage")
                t.image = baked
                t.interpolation = "Linear"
                nt.links.new(uv(nt, "lightmap"), t.inputs["Vector"])
                nt.links.new(t.outputs["Color"], em.inputs["Color"])
                return
            # albedo: the source's base colour, or its texture on UVMap
            p = next(n for n in src.node_tree.nodes if n.bl_idname == "ShaderNodeBsdfPrincipled")
            bc = p.inputs["Base Color"]
            mix = nt.nodes.new("ShaderNodeMix")
            mix.data_type = "RGBA"
            mix.blend_type = "MULTIPLY"
            mix.inputs["Factor"].default_value = 1.0
            if bc.is_linked:
                img = bc.links[0].from_node.image
                t = nt.nodes.new("ShaderNodeTexImage")
                t.image = img
                nt.links.new(uv(nt, "UVMap"), t.inputs["Vector"])
                nt.links.new(t.outputs["Color"], mix.inputs["A"])
            else:
                mix.inputs["A"].default_value = tuple(bc.default_value)
            l = nt.nodes.new("ShaderNodeTexImage")
            l.image = lm
            nt.links.new(uv(nt, "lightmap"), l.inputs["Vector"])
            g = nt.nodes.new("ShaderNodeGamma")
            g.inputs[1].default_value = 2.2
            nt.links.new(l.outputs["Color"], g.inputs[0])
            nt.links.new(g.outputs[0], mix.inputs["B"])
            em.inputs[1].default_value = scale
            nt.links.new(mix.outputs["Result"], em.inputs["Color"])

        m = emission_material("pv_" + ob.name, build)
    else:
        continue
    ob.data.materials.clear()
    ob.data.materials.append(m)

bpy.ops.render.render(write_still=True)
