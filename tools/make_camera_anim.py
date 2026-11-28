"""Build the Camera page's 1st-person hold animation from the vanilla dataslate idle.

Usage:
  py -I tools/make_camera_anim.py <animation io dir> <skeleton.rig> <Idle_DataSlate.af> <out.af> [<arm pose txt>]

<animation io dir> holds CALUMI.Animation.dll and AnimationIO/AnimationIOFuncs.py from the Animation IO Blender addon
(github deveris256/sf_animation_io, rewrite branch); only the ctypes bindings are used, Blender is not needed.

Every block of the source animation is copied. The arm bones (L_Clavicle, L_Biceps, L_Forearm, L_Wrist) get the
rotation offsets tuned in game (defaults/PhoneTest_arm_pose.txt, the format tuner.cpp saves). Without a pose file the
animation is copied unchanged (round-trip test). The .afx sidecar (state, tag PhoneCamera, suffix DataSlate,
Motion Driven like the vanilla idle) is written next to the .af.

Conventions (checked against the 1st-person skeleton.nif and the rig):
- rig / .af quaternion q -> column matrix R(q); the NIF file stores R(q); the game keeps its transpose in memory.
- .af rotation keys are relative to the rig rest pose: local = R(rest) * R(key).
- tuner.cpp writes local_mem = anim_mem * D (D as saved), i.e. local = D^T * anim in column form, so the new key is
  R(rest)^T * D^T * R(rest) * R(key).
"""
import ctypes
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from anim_math import Q, R, T, mul  # noqa: E402

ARM = ["L_Clavicle", "L_Biceps", "L_Forearm", "L_Wrist"]  # tuner.cpp bone order 0..3


def load_pose(path):
    pose = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            p = line.split()
            if len(p) == 11 and p[0] == "bone":
                v = [float(x) for x in p[2:]]
                pose[ARM[int(p[1])]] = [v[0:3], v[3:6], v[6:9]]
    return pose


def main():
    aio, rig_path, src_path, out_path = sys.argv[1:5]
    pose = load_pose(sys.argv[5]) if len(sys.argv) > 5 else {}
    sys.path.insert(0, aio)
    from AnimationIO import AnimationIOFuncs as F

    def quat(p):
        return (F._GetQuaternionXC(p), F._GetQuaternionYC(p), F._GetQuaternionZC(p), F._GetQuaternionWC(p))

    rig = F._LoadSFBGSSkeletonRigFromFileC(ctypes.c_wchar_p(rig_path))
    if not rig:
        raise SystemExit(f"cannot load rig {rig_path}")

    rest = {}

    def walk(b):
        rest[F._GetSkeletonBoneNameC(b).decode()] = quat(F._GetLocalSkeletonBoneRotationC(b))
        i = 0
        while True:
            c = F._GetSkeletonBoneChildC(b, i)
            if not c:
                break
            walk(c)
            i += 1

    walk(F._GetSkeletonRigRootC(rig))

    scene = F._LoadAnimationSceneFromSFBGSFormatC(F._list_to_wchar_arr([rig_path, src_path]), 2)
    src = F._GetAnimationWithIndexC(scene, 0) if scene else None
    if not src:
        raise SystemExit(f"cannot load animation {src_path}")
    title = os.path.splitext(os.path.basename(out_path))[0].lower().encode()
    n_blocks = F._GetAnimationBlockCountC(src)
    out = F._CreateAnimationC(title, n_blocks)
    F._SFBGSAnimationPackage_AddPackageToAnimationC(out, True)

    offset = {}
    for b, D in pose.items():
        Rr = R(rest[b])
        offset[b] = mul(T(Rr), mul(T(D), Rr))

    changed = 0
    copied = 0
    for name in rest:
        blk = F._GetAnimationBlockC(src, name.encode())
        if not blk:
            continue
        nb = F._CreateAnimBlockC(name.encode())
        for k in range(F._GetTranslationSqSizeC(blk)):
            fr = F._GetTranslationFromSqC(blk, k)
            v = F._GetValueFromTranslationFrameC(fr)
            F._AddTranslationSqToAnimBlockC(nb, F._CreateTranslationFrameC(
                F._GetFrameFromTranslationFrameC(fr), F._GetVector3DXC(v), F._GetVector3DYC(v), F._GetVector3DZC(v)), 1, False)
        for k in range(F._GetRotationSqSizeC(blk)):
            fr = F._GetRotationFromSqC(blk, k)
            q = quat(F._GetValueFromRotationFrameC(fr))
            if name in offset:
                q = Q(mul(offset[name], R(q)))
                changed += 1
            F._AddRotationSqToAnimBlockC(nb, F._CreateRotationFrameC(F._GetFrameFromRotationFrameC(fr), *q), 1, False)
        for k in range(F._GetScalarSqSizeC(blk)):
            fr = F._GetScalarFromSqC(blk, k)
            F._AddScalarSqToAnimBlockC(nb, F._CreateScalarFrameC(
                F._GetFrameFromScalarFrameC(fr), F._GetValueFromScalarFrameC(fr)), 1, False)
        for k in range(F._GetPrioritySqSizeC(blk)):
            fr = F._GetPriorityFromSqC(blk, k)
            F._AddPrioritySqToAnimBlockC(nb, F._CreatePriorityFrameC(
                F._GetFrameFromPriorityFrameC(fr), F._GetValueFromPriorityFrameC(fr)), 1, False)
        F._AddAnimBlockToAnimationC(out, nb, True)
        copied += 1

    missing = [b for b in pose if not F._GetAnimationBlockC(src, b.encode())]
    if missing:
        raise SystemExit(f"source animation has no block for {missing}")
    if copied != n_blocks:
        raise SystemExit(f"copied {copied} of {n_blocks} blocks (some block is not a rig bone)")
    rc = F._SaveAnimationToSFBGSFormatWithExistingRigDirectC(out, ctypes.c_wchar_p(out_path), ctypes.c_wchar_p(rig_path))
    print(f"{out_path}: {copied} blocks, {changed} rotation keys posed, save returned {rc}")
    afx = os.path.splitext(out_path)[0] + ".afx"
    name = os.path.basename(out_path)
    tag, _, suffix = os.path.splitext(name)[0].partition("_")
    with open(afx, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("<root>\n\t<is_state>1</is_state>\n"
                f"\t<tag>{tag}</tag>\n\t<filename>{name}</filename>\n\t<suffix>{suffix}</suffix>\n"
                "\t<flags>\n\t\t<flag>Motion Driven</flag>\n\t</flags>\n</root>\n")
    print(f"{afx}: tag {tag}, suffix {suffix}")


if __name__ == "__main__":
    main()
