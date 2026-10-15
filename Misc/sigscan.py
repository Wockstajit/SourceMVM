import sys

def parse_pattern(p):
    out = []
    for tok in p.split():
        out.append(None if tok == '?' or tok == '??' else int(tok, 16))
    return out

def find_all(data, pat, limit=10):
    hits = []
    n = len(pat)
    first = pat[0]
    i = 0
    end = len(data) - n
    while i <= end:
        if data[i] == first:
            ok = True
            for j in range(1, n):
                b = pat[j]
                if b is not None and data[i+j] != b:
                    ok = False
                    break
            if ok:
                hits.append(i)
                if len(hits) >= limit:
                    break
        i += 1
    return hits

path = sys.argv[1]
pattern = sys.argv[2]
with open(path, 'rb') as f:
    data = f.read()
pat = parse_pattern(pattern)
hits = find_all(data, pat)
print(f"{path}: size={len(data)} pattern_len={len(pat)} matches={len(hits)}")
for h in hits:
    print(f"  file_off=0x{h:x} bytes={data[h:h+16].hex()}")
