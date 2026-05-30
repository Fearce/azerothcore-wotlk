import re, sys
BASE = r"D:/WowPs/server-ac/modules/mod-ale/MythicPlus"
SRC = BASE + "/Data/SQL/world/creature_and_keystones.sql"
valid = {
    "creature_template": set(open(BASE + "/cols_creature.txt").read().split()),
    "item_template":     set(open(BASE + "/cols_item.txt").read().split()),
}
txt = open(SRC, encoding="utf-8").read()

def split_top_level(s, sep=','):
    out, buf, depth, i, inq = [], [], 0, 0, False
    while i < len(s):
        c = s[i]
        if inq:
            buf.append(c)
            if c == "'":
                if i+1 < len(s) and s[i+1] == "'":
                    buf.append(s[i+1]); i += 2; continue
                inq = False
            i += 1; continue
        if c == "'":
            inq = True; buf.append(c); i += 1; continue
        if c == '(':
            depth += 1; buf.append(c); i += 1; continue
        if c == ')':
            depth -= 1; buf.append(c); i += 1; continue
        if c == sep and depth == 0:
            out.append(''.join(buf)); buf = []; i += 1; continue
        buf.append(c); i += 1
    out.append(''.join(buf))
    return out

def fix_insert(m):
    table = m.group('table')
    if table not in valid:
        return m.group(0)
    cols = [c.strip().strip('`') for c in m.group('cols').split(',')]
    keep = [i for i,c in enumerate(cols) if c in valid[table]]
    dropped = [c for c in cols if c not in valid[table]]
    new_cols = ", ".join("`%s`" % cols[i] for i in keep)
    body = m.group('vals').strip().rstrip(';').strip()
    tuples = split_top_level(body)
    new_tuples = []
    for t in tuples:
        t = t.strip()
        if not (t.startswith('(') and t.endswith(')')):
            new_tuples.append(t); continue
        vals = split_top_level(t[1:-1])
        if len(vals) != len(cols):
            sys.stderr.write("WARN %s: %d vals vs %d cols\n" % (table, len(vals), len(cols)))
        new_vals = ", ".join(vals[i].strip() for i in keep if i < len(vals))
        new_tuples.append("(%s)" % new_vals)
    sys.stderr.write("%s: dropped %s\n" % (table, dropped))
    return "INSERT INTO `%s` (%s) VALUES\n%s;" % (table, new_cols, ",\n".join(new_tuples))

pat = re.compile(
    r"INSERT INTO `(?P<table>creature_template|item_template)`\s*\((?P<cols>[^)]*)\)\s*VALUES\s*(?P<vals>.*?);",
    re.DOTALL)
out = pat.sub(fix_insert, txt)
open(SRC, "w", encoding="utf-8").write(out)
print("rewrote", SRC)
