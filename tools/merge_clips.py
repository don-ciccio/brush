# Headless Blender: merge selected animation clips from a donor GLB onto the
# base mannequin GLB (identical skeleton required — same joint names/order;
# UAL1 and UAL2 share the Quaternius universal rig).
# Usage: Blender -b --python merge_clips.py -- <base.glb> <donor.glb> <out.glb> <Clip1,Clip2,...>
import bpy
import sys

argv = sys.argv[sys.argv.index("--") + 1:]
base, donor, dst = argv[0], argv[1], argv[2]
TAKE = set(argv[3].split(","))

# Fresh scene, import the base (its actions are the ones we keep as-is).
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=base)
base_objs = set(bpy.data.objects)
base_actions = set(bpy.data.actions)
arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
print("BASE_ACTIONS:", sorted(a.name for a in base_actions))

# Import the donor; its actions target the same bone names so they apply to
# the base armature directly. Drop everything not in TAKE, plus the donor's
# scene objects (we only want the actions).
bpy.ops.import_scene.gltf(filepath=donor)
donor_actions = [a for a in bpy.data.actions if a not in base_actions]
taken = []
for a in donor_actions:
    if a.name in TAKE:
        taken.append(a.name)
    else:
        bpy.data.actions.remove(a)
for o in [o for o in bpy.data.objects if o not in base_objs]:
    bpy.data.objects.remove(o, do_unlink=True)
missing = TAKE - set(taken)
if missing:
    print("MISSING_ACTIONS:", sorted(missing))
    sys.exit(1)
print("TAKEN_ACTIONS:", sorted(taken))

# Push every action onto the base armature NLA so the glTF exporter writes
# all of them (it exports actions referenced by NLA tracks).
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
