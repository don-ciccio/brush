# Headless Blender: import UAL1_Standard.glb, keep only the core locomotion
# actions, re-export a small GLB for the brush sandbox.
# Usage: Blender -b --python trim_ual.py -- <in.glb> <out.glb>
import bpy
import sys

argv = sys.argv[sys.argv.index("--") + 1:]
src, dst = argv[0], argv[1]

KEEP = {
    "Idle_Loop",
    "Walk_Loop",
    "Jog_Fwd_Loop",
    "Sprint_Loop",
    "Jump_Start",
    "Jump_Loop",
    "Jump_Land",
    "Crouch_Idle_Loop",
    "Crouch_Fwd_Loop",
    "Roll",
}

# Fresh scene
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)

names = sorted(a.name for a in bpy.data.actions)
print("IMPORTED_ACTIONS:", len(names))

removed = 0
for a in list(bpy.data.actions):
    if a.name not in KEEP:
        bpy.data.actions.remove(a)
        removed += 1
kept = sorted(a.name for a in bpy.data.actions)
print("KEPT_ACTIONS:", kept, "removed:", removed)
missing = KEEP - set(kept)
if missing:
    print("MISSING_ACTIONS:", sorted(missing))
    sys.exit(1)

# Push every kept action onto the armature NLA so the glTF exporter writes all
# of them (it exports actions referenced by NLA tracks).
arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
if arm.animation_data is None:
    arm.animation_data_create()
for track in list(arm.animation_data.nla_tracks):
    arm.animation_data.nla_tracks.remove(track)
for a in bpy.data.actions:
    tr = arm.animation_data.nla_tracks.new()
    tr.name = a.name
    tr.strips.new(a.name, int(a.frame_range[0]), a)
arm.animation_data.action = None

bpy.ops.export_scene.gltf(
    filepath=dst,
    export_format="GLB",
    export_animations=True,
    export_animation_mode="ACTIONS",
    export_skins=True,
    export_yup=True,
)
print("EXPORTED:", dst)
