#!/usr/bin/env python3
"""Match decompiled functions between two builds of the same driver.

Input is the output of DumpAll.java for each build.  Functions are paired by
(1) identical normalized body, (2) identical set of referenced strings,
(3) call-graph propagation from already-paired callers, and (4) best fuzzy
body similarity >= 0.85 among functions of similar size.

usage: match_builds.py old.c new.c [FUN_<addr> ...]

Writes <new>.report.txt (summary, changed/unmatched functions, status of the
watched old-build addresses) and one unified diff per changed pair into
<new>.diffs/.
"""
import difflib
import hashlib
import os
import re
import sys
from collections import defaultdict

ADDR_TOKENS = re.compile(
    r"\b(FUN|DAT|LAB|PTR|switchD|caseD|thunk_FUN|s_\w*?|u_\w*?|PTR_FUN|"
    r"code_r0x|joined_r0x|_DAT)_[0-9a-fA-F_]+\w*")


def normalize(src):
    """Strip addresses and decompiler naming noise that differ between builds."""
    s = ADDR_TOKENS.sub(lambda m: m.group(1) + "_X", src)
    s = re.sub(r"0x14[0-9a-f]{7}\b", "ADDR", s)
    s = re.sub(r"\b(local|param|in_stack|auStack|uStack|lStack|iStack|puStack|"
               r"plStack|pcStack|abStack|bStack|cStack|sStack|in_[A-Z]+)_?"
               r"[0-9a-fA-F]+\b", r"\1_N", s)
    s = re.sub(r"\b([a-z]{1,3}Var)\d+\b", r"\1N", s)
    s = re.sub(r"\b(extraout|unaff|in)_([A-Za-z0-9]+)\b", r"\1_R", s)
    s = re.sub(r"[ \t]+", " ", s)
    return "\n".join(l.strip() for l in s.splitlines() if l.strip())


def parse(path):
    funcs = {}
    cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("@@FUNC "):
            parts = line.split()
            cur = {"size": int(parts[2].split("=")[1]), "strs": [], "body": []}
            funcs[parts[1]] = cur
        elif cur is not None and line.startswith("@@STR "):
            cur["strs"].append(line[6:].rstrip("\n"))
        elif cur is not None:
            cur["body"].append(line)
    for f in funcs.values():
        f["raw"] = "".join(f["body"])
        f["norm"] = normalize(f["raw"])
        f["hash"] = hashlib.sha1(f["norm"].encode()).hexdigest()
        f["skey"] = "\x00".join(sorted(set(f["strs"])))
    return funcs


def match(old, new):
    pairs = {}   # old addr -> new addr
    how = {}

    def uniq_index(funcs, key):
        idx = defaultdict(list)
        for a, f in funcs.items():
            if f[key]:
                idx[f[key]].append(a)
        return {k: v[0] for k, v in idx.items() if len(v) == 1}

    # 1 + 2: unique identical body, then unique identical string set.
    for key, label in (("hash", "identical"), ("skey", "strings")):
        oi = uniq_index({a: f for a, f in old.items() if a not in pairs}, key)
        used = set(pairs.values())
        ni = uniq_index({a: f for a, f in new.items() if a not in used}, key)
        for k, oa in oi.items():
            if k in ni:
                pairs[oa] = ni[k]
                how[oa] = label

    # 3: callees in the same position of paired callers, until stable.
    call_re = re.compile(r"\bFUN_([0-9a-f]+)\s*\(")
    changed = True
    while changed:
        changed = False
        used = set(pairs.values())
        for oa, na in list(pairs.items()):
            oc = [c for c in call_re.findall(old[oa]["raw"]) if c in old]
            nc = [c for c in call_re.findall(new[na]["raw"]) if c in new]
            if len(oc) != len(nc):
                continue
            for o, n in zip(oc, nc):
                if o not in pairs and n not in used:
                    pairs[o] = n
                    how[o] = "callgraph"
                    used.add(n)
                    changed = True

    # 4: fuzzy, among remaining functions of similar size.
    used = set(pairs.values())
    cand_new = [a for a in new if a not in used]
    for oa in sorted(a for a in old if a not in pairs):
        o = old[oa]
        best = (0.0, None)
        for na in cand_new:
            n = new[na]
            if na in used or abs(n["size"] - o["size"]) > max(16, o["size"] * 0.3):
                continue
            sm = difflib.SequenceMatcher(None, o["norm"], n["norm"])
            if sm.real_quick_ratio() < best[0] or sm.quick_ratio() < best[0]:
                continue
            r = sm.ratio()
            if r > best[0]:
                best = (r, na)
        if best[1] and best[0] >= 0.85:
            pairs[oa] = best[1]
            how[oa] = f"fuzzy{best[0]:.2f}"
            used.add(best[1])
    return pairs, how


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    old_path, new_path = sys.argv[1], sys.argv[2]
    old_tag = os.path.basename(old_path)
    new_tag = os.path.basename(new_path)
    old, new = parse(old_path), parse(new_path)
    pairs, how = match(old, new)

    outdir = new_path + ".diffs"
    os.makedirs(outdir, exist_ok=True)
    same = 0
    changed_list = []
    for oa, na in sorted(pairs.items()):
        o, n = old[oa], new[na]
        if o["hash"] == n["hash"]:
            same += 1
            continue
        ratio = difflib.SequenceMatcher(None, o["norm"], n["norm"]).ratio()
        changed_list.append((ratio, oa, na))
        with open(os.path.join(outdir, f"{oa}_{na}.diff"), "w") as fh:
            fh.writelines(difflib.unified_diff(
                o["norm"].splitlines(True), n["norm"].splitlines(True),
                f"{old_tag}/{oa}", f"{new_tag}/{na}", n=3))

    um_old = sorted(set(old) - set(pairs))
    um_new = sorted(set(new) - set(pairs.values()))
    rep = [f"old={len(old)} new={len(new)} matched={len(pairs)} "
           f"identical={same} changed={len(changed_list)} "
           f"unmatched_old={len(um_old)} unmatched_new={len(um_new)}"]
    rep.append("\n== changed (ratio, old -> new, how, size old/new, strings) ==")
    for ratio, oa, na in sorted(changed_list):
        rep.append(f"{ratio:.3f} {oa} -> {na} [{how[oa]}] "
                   f"{old[oa]['size']}/{new[na]['size']} "
                   f"{'; '.join(old[oa]['strs'])[:160]}")
    rep.append(f"\n== unmatched in old (only in {old_tag}) ==")
    for a in um_old:
        rep.append(f"{a} size={old[a]['size']} {'; '.join(old[a]['strs'])[:160]}")
    rep.append(f"\n== unmatched in new (only in {new_tag}) ==")
    for a in um_new:
        rep.append(f"{a} size={new[a]['size']} {'; '.join(new[a]['strs'])[:160]}")
    rep.append("\n== watched old-build functions ==")
    for w in sys.argv[3:]:
        w = w.replace("FUN_", "")
        if w not in old:
            continue
        if w in pairs:
            na = pairs[w]
            st = "IDENTICAL" if old[w]["hash"] == new[na]["hash"] else "CHANGED"
            rep.append(f"{w} -> {na} [{how[w]}] {st}")
        else:
            rep.append(f"{w} -> UNMATCHED")
    with open(new_path + ".report.txt", "w") as fh:
        fh.write("\n".join(rep) + "\n")
    print(rep[0])


if __name__ == "__main__":
    main()
