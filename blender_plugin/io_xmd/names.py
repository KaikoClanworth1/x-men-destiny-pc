# Names for hashed identifiers.  The game stores only hashes (lookup3 of the upper-cased
# name); names are recovered by hashing likely candidates - standard 3ds Max Biped bone
# names and common material slot words.  By KaikoClanworth
from .skhash import h

_bones = None
_slots = None


def _bone_candidates():
    out = ["Root", "root", "Bip01", "Bip01 Footsteps", "Bip01 Pelvis", "Bip01 Spine", "Bip01 Spine1",
           "Bip01 Spine2", "Bip01 Spine3", "Bip01 Neck", "Bip01 Neck1", "Bip01 Neck2", "Bip01 Head",
           "Bip01 HeadNub", "Bip01 Tail", "Bip01 Tail1", "Bip01 Tail2", "Bip01 Tail3", "Bip01 TailNub",
           "Bip01 Ponytail1", "Bip01 Ponytail11", "Bip01 Ponytail12", "Bip01 Ponytail1Nub",
           "Bip01 Ponytail2", "Bip01 Ponytail21", "Bip01 Ponytail22", "Bip01 Ponytail2Nub",
           "Bip01 Prop1", "Bip01 Prop2", "Bip01 Prop3", "Scene Root", "Camera", "Trajectory",
           "Reference", "Ground", "Motion", "Origin", "World", "Dummy", "Jaw", "Bone_Jaw", "Head", "Eye_L",
           "Eye_R", "L_Eye", "R_Eye", "Bone_L_Eye", "Bone_R_Eye", "Weapon", "Weapon_L", "Weapon_R"]
    limb = ["Clavicle", "UpperArm", "Forearm", "Hand", "Thigh", "Calf", "Foot", "Toe0", "Toe0Nub",
            "Toe01", "Toe02", "HorseLink", "UpArmTwist", "UpArmTwist1", "UpArmTwist2", "ForeTwist",
            "ForeTwist1", "ForeTwist2", "ThighTwist", "ThighTwist1", "ThighTwist2", "CalfTwist",
            "CalfTwist1", "Collar", "Collarbone"]
    for f in range(5):
        limb += ["Finger%d" % f, "Finger%d1" % f, "Finger%d2" % f, "Finger%dNub" % f,
                 "Toe%d" % f, "Toe%d1" % f, "Toe%d2" % f, "Toe%dNub" % f]
    for side in ("L", "R"):
        for l in limb:
            out.append("Bip01 %s %s" % (side, l))
            out.append("Bip01 %s%s" % (side, l))
    return out


def bone_name(hash_value):
    global _bones
    if _bones is None:
        _bones = {}
        for n in _bone_candidates():
            _bones.setdefault(h(n), n)
    return _bones.get(hash_value) or "bone_%08X" % hash_value


def _slot_candidates():
    """Material texture slots are free-form artist names; these are the common patterns."""
    bases = ["diff", "diffuse", "dif", "colour", "color", "albedo", "spec", "specular", "gloss", "norm",
             "normal", "nrm", "bump", "mask", "height", "emis", "emissive", "glow", "alpha", "opacity",
             "reflection", "reflect", "refl", "env", "cube", "cubemap", "envmap", "detail", "noise",
             "lightmap", "ao", "ramp", "smoke", "wood", "specnoise", "whiteband", "diff_eye",
             "eye_lens_normal", "glow_glass_vertex", "reflectionmask", "reflection_mask",
             "reflection_texture", "detail_normal", "detailnormal", "normal_detail", "diffuse_detail"]
    ends = ["", "map", "_map", "_tex", "_texture", "_mask", "_damaged", "_noise", "_detail"]
    nums = [""] + [str(i) for i in range(10)] + ["%02d" % i for i in range(10)] + ["_%d" % i for i in range(1, 5)] \
        + ["_%02d" % i for i in range(1, 5)]
    out = []
    for pre in ("", "tex_", "t_"):
        for b in bases:
            for n in nums:
                for e in ends:
                    out.append(pre + b + n + e)
                    if n and e:
                        out.append(pre + b + e + n)
    return out


def slot_name(hash_value):
    global _slots
    if _slots is None:
        _slots = {}
        for n in _slot_candidates():
            _slots.setdefault(h(n), n)
    return _slots.get(hash_value) or "slot_%08X" % hash_value
