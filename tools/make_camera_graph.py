"""Add the phone's Camera hold state to the vanilla 1st-person animation data.

Usage: py -I tools/make_camera_graph.py <vanilla AnimTextData dir> <out AnimTextData dir>

The vanilla dir is Meshes/AnimTextData extracted from Starfield - Animations.ba2 (needs AnimationFiles.txt,
Tables/anim_events.xtbl, Tables/anim_states.xtbl, Tables/Graphs/Human_1stPersonRoot.agx). Written, same layout:
- Tables/Graphs/Human_1stPersonRoot.agx: the graph with the new state (below)
- Tables/anim_events.xtbl: PhoneCameraIn / PhoneCameraOut declared (the engine rejects undeclared events; the
  ChronoMark MK II mod ships the same pair of files for its 1st-person state)
- Tables/anim_states.xtbl: tag PhoneCamera declared next to Idle
- AnimationFiles.txt: the new .af / .afx listed (every animation file the game loads must be listed there)

The dataslate / digipick state machine (Equip -> Idle -> Unequip -> None) gets a fifth state, PhoneCamera
(tag PhoneCamera; with the DataSlate suffix the graph plays PhoneCamera_DataSlate.af):
- Idle -> PhoneCamera on event PhoneCameraIn
- PhoneCamera -> Idle on event PhoneCameraOut
- PhoneCamera -> Unequip on event DataSlateClose (closing the phone from the Camera page)
All blends 0.3 s like the vanilla transitions. Nodes are found by their vanilla GUIDs; the script stops if the graph
does not look like the one it was written for.
"""
import re
import sys

IDLE = "e1c285ac-59d8-497b-a051-f77c15f047a9"
UNEQUIP = "4687f582-42b4-416e-8342-96c7f089f1f0"
NONE = "d934e09f-0a60-41f9-9653-a36b2a95b381"
CAMERA = "6b0f3c52-7d1e-4a8e-9f61-2c5a0e7d4b19"  # new
TAG = "PhoneCamera"


def node_span(t, guid):
    g = t.index(f"<guid>{guid}</guid>")
    start = t.rindex("\n<node>\n", 0, g) + 1  # node elements start / end at column 0; links use <node>N</node>
    end = t.index("\n</node>\n", g) + len("\n</node>")
    return start, end


def one(t, old, new):
    if t.count(old) != 1:
        raise SystemExit(f"expected one {old!r}, found {t.count(old)}")
    return t.replace(old, new)


def input_block(nl, in_id, idx, src_node, src_out, event, template):
    b = template
    b = re.sub(r"<input> <name></name> <id>\d+</id> <idx>\d+</idx>", f"<input> <name></name> <id>{in_id}</id> <idx>{idx}</idx>", b)
    b = re.sub(r"<node>\d+</node> <output>\d+</output>", f"<node>{src_node}</node> <output>{src_out}</output>", b)
    ev = "<prop> <type>2</type> <value>Event</value> </prop>"
    i = b.index(ev) + len(ev)
    j = b.index("</value>", i)
    k = b.rindex("<value>", i, j) + len("<value>")
    return b[:k] + event + b[j:]


def graph(src, dst):
    raw = open(src, "rb").read().decode("utf-8")
    nl = "\r\n" if "\r\n" in raw else "\n"
    t = raw.replace("\r\n", "\n")

    # the state machine must still be the vanilla one: four states, user ids 0..3
    s, e = node_span(t, IDLE)
    idle = t[s:e]
    if "<user_id>1</user_id>" not in idle or "<value>Idle</value>" not in idle:
        raise SystemExit("Idle state not found where expected")
    m = re.search(r"\t<input> <name></name> <id>1</id> <idx>0</idx>\n.*?\n </input>\n", idle, re.S)
    template = m.group(0)
    if "<node>1</node> <output>0</output>" not in template:
        raise SystemExit("Idle's input is not the Equip transition")

    # the new state: a copy of Idle with its own tag, id and incoming transition from Idle (node 2, output 2)
    cam = idle
    cam = one(cam, f"<guid>{IDLE}</guid>", f"<guid>{CAMERA}</guid>")
    cam = one(cam, "<user_id>1</user_id>", "<user_id>4</user_id>")
    cam = re.sub(r"<pos_x>\d+</pos_x>", "<pos_x>31</pos_x>", cam)
    cam = re.sub(r"<pos_y>\d+</pos_y>", "<pos_y>330</pos_y>", cam)
    cam = re.sub(r"<expanded_pos_x>\d+</expanded_pos_x>", "<expanded_pos_x>31</expanded_pos_x>", cam)
    cam = re.sub(r"<expanded_pos_y>\d+</expanded_pos_y>", "<expanded_pos_y>330</expanded_pos_y>", cam)
    cam = one(cam, "<value>TagName</value> </prop>\n\t\t\t<prop> <type>2</type> <value>Idle</value>",
              f"<value>TagName</value> </prop>\n\t\t\t<prop> <type>2</type> <value>{TAG}</value>")
    cam = one(cam, template, input_block(nl, 1, 0, 2, 2, "PhoneCameraIn", template))
    # outputs id 2 (to Idle) and id 3 (to Unequip) are inherited from the copy

    # Idle: a third output (to PhoneCamera) and an input from PhoneCamera (node 5, output 0)
    out3 = "\t<output> <name></name> <id>3</id> <idx>1</idx>\n </output>\n"
    new_idle = idle.replace(template, template + input_block(nl, 5, 1, 5, 0, "PhoneCameraOut", template), 1)
    new_idle = one(new_idle, out3, out3 + "\t<output> <name></name> <id>4</id> <idx>2</idx>\n </output>\n")
    t = t[:s] + new_idle + t[e:]

    # Unequip: an input from PhoneCamera (node 5, output 1) on DataSlateClose
    s, e = node_span(t, UNEQUIP)
    uneq = t[s:e]
    last_in = uneq.rindex(" </input>\n") + len(" </input>\n")
    uneq = uneq[:last_in] + input_block(nl, 4, 2, 5, 1, "DataSlateClose", template) + uneq[last_in:]
    t = t[:s] + uneq + t[e:]

    # the new node goes after None (5th node of the embedded graph)
    s, e = node_span(t, NONE)
    t = t[:e] + "\n" + cam + t[e:]

    # state table row
    row_none = ("\t\t<row>\n\t\t\t<prop> <type>2</type> <value></value> </prop>\n"
                "\t\t\t<prop> <type>2</type> <value>None</value> </prop>\n"
                "\t\t\t<prop> <type>0</type> <value>3</value> </prop>\n\t\t</row>\n")
    t = one(t, row_none, row_none + row_none.replace("<value>None</value>", f"<value>{TAG}</value>").replace(
        "<value>3</value>", "<value>4</value>"))

    open(dst, "wb").write(t.replace("\n", nl).encode("utf-8"))
    print(f"{dst}: state {TAG} added (Idle <-> {TAG} on PhoneCameraIn / PhoneCameraOut, {TAG} -> Unequip on DataSlateClose)")


ANIM_DIR = "Meshes\\Actors\\Human\\_1stPerson\\Animations\\Common\\"
NEW_FILES = [ANIM_DIR + f"{TAG}_DataSlate.af", ANIM_DIR + f"{TAG}_DataSlate.afx"]


def index(src, dst):
    raw = open(src, "rb").read().decode("utf-8")
    nl = "\r\n" if "\r\n" in raw else "\n"
    lines = raw.split(nl)
    tail = [] if lines[-1] else [""]
    lines = [x for x in lines if x]
    have = {x.lower() for x in lines}
    for new in NEW_FILES:
        if new.lower() in have:
            continue
        # keep the vanilla order: before the first later name in the same folder
        pos = None
        for i, x in enumerate(lines):
            if x.lower().startswith(ANIM_DIR.lower()):
                if pos is None:
                    pos = i + 1
                if x.lower() < new.lower():
                    pos = i + 1
        if pos is None:
            raise SystemExit(f"index has no {ANIM_DIR} entries")
        lines.insert(pos, new)
    open(dst, "wb").write(nl.join(lines + tail).encode("utf-8"))
    print(f"{dst}: {len(lines)} entries ({', '.join(NEW_FILES)})")


def table(src, dst, entries, key):
    """Add entries to a sorted .xtbl table; key = the tag holding the name (name0 for events, name for states)."""
    raw = open(src, "rb").read().decode("utf-8")
    nl = "\r\n" if "\r\n" in raw else "\n"
    t = raw.replace("\r\n", "\n")
    rows = re.findall(r"\t<entry>.*?</entry>\n", t)
    names = [re.search(f"<{key}>(.*?)</{key}>", r).group(1) for r in rows]
    for name, row in entries:
        if name in names:
            continue
        later = [i for i, n in enumerate(names) if n.lower() > name.lower()]
        anchor = rows[later[0]] if later else None
        if anchor:
            t = one(t, anchor, row + "\n" + anchor)
        else:
            t = one(t, "</table>", row + "\n</table>")
        rows = re.findall(r"\t<entry>.*?</entry>\n", t)
        names = [re.search(f"<{key}>(.*?)</{key}>", r).group(1) for r in rows]
    open(dst, "wb").write(t.replace("\n", nl).encode("utf-8"))
    print(f"{dst}: {', '.join(n for n, _ in entries)}")


def main():
    import os
    src, dst = sys.argv[1:3]
    for d in ("Tables/Graphs",):
        os.makedirs(os.path.join(dst, d), exist_ok=True)
    graph(os.path.join(src, "Tables/Graphs/Human_1stPersonRoot.agx"), os.path.join(dst, "Tables/Graphs/Human_1stPersonRoot.agx"))
    events = [(e, f"\t<entry><name0>{e}</name0><name1>Both</name1><name2>True</name2></entry>")
              for e in ("PhoneCameraIn", "PhoneCameraOut")]  # like DataSlateClose
    table(os.path.join(src, "Tables/anim_events.xtbl"), os.path.join(dst, "Tables/anim_events.xtbl"), events, "name0")
    table(os.path.join(src, "Tables/anim_states.xtbl"), os.path.join(dst, "Tables/anim_states.xtbl"),
          [(TAG, f"\t<entry><name>{TAG}</name></entry>")], "name")
    index(os.path.join(src, "AnimationFiles.txt"), os.path.join(dst, "AnimationFiles.txt"))


if __name__ == "__main__":
    main()
