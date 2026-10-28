# make_tux.py -- the GPU demo's scene (teutekeune's Tux, tux.blend) made
# ready for real-time rendering on the iPad's SGX: run headless on a COPY of
# the artist's file, the results written beside it (tux-rt.blend saved
# apart, the copy as it was), then tux.glb and tux-baked.png into assets/:
#
#   cp tux.blend build/tux/tux-src.blend
#   Blender -b build/tux/tux-src.blend --python tools/sgx/tux/make_tux.py
#   (TUX_LIGHTMAP=2048, TUX_SAMPLES=512: about 3.5 minutes on an M5 Pro's GPU)
#
# - the Apple logo's glass (lit by two point lights inside it) lights the
#   bake as in the render, then becomes an emissive material for the glb;
# - the belly (no material: Blender's default grey) gets one;
# - rotation and scale applied (one foot is mirrored by a negative scale),
#   normals made to point out, the camera's scale reset;
# - a second UV map, "lightmap", one atlas for every lit object;
# - Cycles bakes the diffuse light (direct + indirect, no albedo) into it --
#   the PNG stores (L / scale) ^ (1 / 2.2), scale in the glb's scene extras
#   -- and the whole colour as the render has it (tux-baked.png, AgX);
# - tux.glb: the lit objects, the logo and the camera (no lights, no
#   backdrop, no reference images); the working file saved beside it.
import json, math, os

import bmesh
import bpy
import numpy as np

OUT = os.path.dirname(os.path.abspath(bpy.data.filepath))
SIZE = int(os.environ.get("TUX_LIGHTMAP", "2048"))
SAMPLES = int(os.environ.get("TUX_SAMPLES", "512"))
LOGO = ("Sphere.009", "Sphere.010")
BELLY = "Sphere.008"
BACKDROP = "Cube.001"

scene = bpy.context.scene
view_layer = bpy.context.view_layer


def log(*a):
    print("make_tux:", *a, flush=True)


def select_only(objs, active=None):
    bpy.ops.object.mode_set(mode="OBJECT") if bpy.context.object and bpy.context.object.mode != "OBJECT" else None
    for o in view_layer.objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    view_layer.objects.active = active or (objs[0] if objs else None)


def principled(name, base, rough=1.0, emission=None, strength=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    p = nt.nodes.new("ShaderNodeBsdfPrincipled")
    p.inputs["Base Color"].default_value = (*base, 1.0)
    p.inputs["Roughness"].default_value = rough
    if emission:
        p.inputs["Emission Color"].default_value = (*emission, 1.0)
        p.inputs["Emission Strength"].default_value = strength
    nt.links.new(p.outputs["BSDF"], out.inputs["Surface"])
    return m


# ---- the GPU (Metal) for Cycles ----------------------------------------------
prefs = bpy.context.preferences.addons["cycles"].preferences
try:
    prefs.compute_device_type = "METAL"
    prefs.get_devices()
    for d in prefs.devices:
        d.use = True
    scene.cycles.device = "GPU"
    log("devices", [(d.name, d.type) for d in prefs.devices if d.use])
except Exception as e:  # noqa: BLE001
    log("no GPU, the CPU then:", e)
    scene.cycles.device = "CPU"

obj = {o.name: o for o in scene.objects}
meshes = [o for o in scene.objects if o.type == "MESH" and o.name != BACKDROP]
logo = [obj[n] for n in LOGO]
lit = [o for o in meshes if o.name not in LOGO]

# ---- materials ---------------------------------------------------------------
# (the logo keeps its glass for the bake -- the light comes out of it as in
# the render -- and turns emissive just before the export, below: an
# emissive logo baked as a lamp of its own made the belly twice as bright,
# and one seen by no ray let all of the lamps' light through)
emit = principled("logo_emission", (1.0, 1.0, 1.0), 1.0, (1.0, 1.0, 1.0), 5.0)
belly = principled("belly", (0.8, 0.8, 0.8), 1.0)
obj[BELLY].data.materials.clear()
obj[BELLY].data.materials.append(belly)
log("belly", belly.name)

# ---- transforms, normals, camera -----------------------------------------------
select_only(meshes)
bpy.ops.object.make_single_user(object=True, obdata=True)
bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
for o in meshes:
    bm = bmesh.new()
    bm.from_mesh(o.data)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(o.data)
    bm.free()
    o.data.update()
cam = scene.camera
cam.scale = (1.0, 1.0, 1.0)
log("transforms applied; camera", cam.name, "lens", round(cam.data.lens, 1))

# ---- the lightmap UV atlas ------------------------------------------------------
for o in lit:
    uv = o.data.uv_layers.get("lightmap") or o.data.uv_layers.new(name="lightmap")
    o.data.uv_layers.active = uv                       # what the unwrap and the bake write
    o.data.uv_layers["UVMap"].active_render = True     # what the textures read
select_only(lit)
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="SELECT")
bpy.ops.uv.smart_project(angle_limit=math.radians(45), island_margin=0.0, area_weight=0.0,
                         correct_aspect=True, scale_to_bounds=False)
bpy.ops.uv.pack_islands(rotate=True, margin=8.0 / SIZE)
bpy.ops.object.mode_set(mode="OBJECT")
log("lightmap UVs for", len(lit), "objects")

# ---- the bake -------------------------------------------------------------------
hdr = bpy.data.images.new("lightmap_hdr", SIZE, SIZE, alpha=True, float_buffer=True)
hdr.pixels.foreach_set(np.zeros(SIZE * SIZE * 4, dtype=np.float32))   # alpha 0: not baked
bake_nodes = []
for m in {s.material for o in lit for s in o.material_slots if s.material}:
    n = m.node_tree.nodes.new("ShaderNodeTexImage")
    n.image = hdr
    m.node_tree.nodes.active = n
    bake_nodes.append((m, n))
scene.render.engine = "CYCLES"
scene.cycles.samples = SAMPLES
bake = scene.render.bake
bake.use_pass_direct = True
bake.use_pass_indirect = True
bake.use_pass_color = False
bake.margin = 16
bake.use_clear = False
bake.target = "IMAGE_TEXTURES"
select_only(lit)
log("baking", SIZE, "x", SIZE, "at", SAMPLES, "samples")
bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"}, margin=16, use_clear=False)
for m, n in bake_nodes:
    m.node_tree.nodes.remove(n)

px = np.empty(SIZE * SIZE * 4, dtype=np.float32)
hdr.pixels.foreach_get(px)
px = px.reshape(SIZE, SIZE, 4)
covered = px[..., 3] > 0
peak = px[..., :3].max(axis=2)[covered]
scale = float(np.percentile(peak, 99.5)) if peak.size else 1.0
log("covered texels %.1f%%, light max %.3f, 99.5%% %.3f" % (100 * covered.mean(), float(peak.max()), scale))
hdr.filepath_raw = os.path.join(OUT, "tux-lightmap.exr")
hdr.file_format = "OPEN_EXR"
hdr.save()

enc = np.clip(px[..., :3] / scale, 0.0, 1.0) ** (1.0 / 2.2)
ldr = bpy.data.images.new("lightmap", SIZE, SIZE, alpha=False)
ldr.colorspace_settings.name = "Non-Color"
out = np.ones((SIZE, SIZE, 4), dtype=np.float32)
out[..., :3] = enc
ldr.pixels.foreach_set(out.ravel())
ldr.filepath_raw = os.path.join(OUT, "tux-lightmap.png")
ldr.file_format = "PNG"
ldr.save()

# ---- the colour as the render has it -----------------------------------------------
# The body is black (base colour 0): its form is the rough dielectric's sheen,
# which a diffuse lightmap has not. So the whole shading too -- light, sheen,
# albedo -- baked into the same UVs and saved through the scene's view
# transform (AgX), display-referred: drawn as it is, unlit, it looks like
# the render (the sheen as seen along each surface's normal).
col = bpy.data.images.new("baked_hdr", SIZE, SIZE, alpha=True, float_buffer=True)
col.pixels.foreach_set(np.zeros(SIZE * SIZE * 4, dtype=np.float32))
bake_nodes = []
for m in {s.material for o in lit for s in o.material_slots if s.material}:
    n = m.node_tree.nodes.new("ShaderNodeTexImage")
    n.image = col
    m.node_tree.nodes.active = n
    bake_nodes.append((m, n))
select_only(lit)
log("baking the colour (COMBINED)")
bpy.ops.object.bake(type="COMBINED", margin=16, use_clear=False,
                    pass_filter={"EMIT", "DIRECT", "INDIRECT", "COLOR", "DIFFUSE", "GLOSSY",
                                 "TRANSMISSION"})
for m, n in bake_nodes:
    m.node_tree.nodes.remove(n)
col.filepath_raw = os.path.join(OUT, "tux-baked.exr")
col.file_format = "OPEN_EXR"
col.save()
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGB"
scene.render.image_settings.color_depth = "8"
col.save_render(os.path.join(OUT, "tux-baked.png"), scene=scene)

# ---- the glb ---------------------------------------------------------------------
for o in logo:
    o.data.materials.clear()
    o.data.materials.append(emit)
log("logo emissive")
scene["baked_file"] = "tux-baked.png"
scene["baked_note"] = ("the lit objects' whole colour on texcoord 1, display-referred (AgX, as "
                       "the render): drawn unlit as it is")
scene["lightmap_file"] = "tux-lightmap.png"
scene["lightmap_texcoord"] = 1
scene["lightmap_scale"] = scale
scene["lightmap_decode"] = "light = lightmap_scale * pow(png.rgb, 2.2); colour = albedo * light"
scene["lightmap_note"] = ("diffuse light baked by Cycles, no albedo; the logo (logo_emission) "
                          "is unlit, emissive 1.0 x strength 5")
select_only(lit + logo + [cam])
gltf = bpy.ops.export_scene.gltf
props = set(gltf.get_rna_type().properties.keys())
want = dict(filepath=os.path.join(OUT, "tux.glb"), export_format="GLB", use_selection=True,
            export_apply=True, export_texcoords=True, export_normals=True,
            export_materials="EXPORT", export_cameras=True, export_lights=False,
            export_extras=True, export_yup=True, export_draco_mesh_compression_enable=False)
gltf(**{k: v for k, v in want.items() if k in props or k == "filepath"})
log("exported", want["filepath"], "skipped options:", sorted(k for k in want if k not in props and k != "filepath"))

bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, "tux-rt.blend"))
json.dump({"lightmap_scale": scale, "size": SIZE, "samples": SAMPLES},
          open(os.path.join(OUT, "bake.json"), "w"), indent=1)
log("done")
