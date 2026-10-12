# Makes the files here from GeoNames' and Natural Earth's downloads (see ATTRIBUTION.txt for where).
#   make.py <download folder> <output folder>
import io, os, struct, sys, zipfile

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)

# Countries: ISO code -> name.
countries = {}
for line in open(os.path.join(src, "countryInfo.txt"), encoding="utf-8"):
    if line.startswith("#"):
        continue
    f = line.rstrip("\n").split("\t")
    if len(f) > 4:
        countries[f[0]] = f[4]
with open(os.path.join(out, "countries.tsv"), "w", encoding="utf-8") as o:
    for code in sorted(countries):
        o.write(f"{code}\t{countries[code]}\n")

# Regions (admin1): "GB.ENG" -> "England".
regions = {}
for line in open(os.path.join(src, "admin1CodesASCII.txt"), encoding="utf-8"):
    f = line.rstrip("\n").split("\t")
    if len(f) >= 2:
        regions[f[0]] = f[1]

# Cities over 15000 people: name, country, region, latitude, longitude.
z = zipfile.ZipFile(os.path.join(src, "cities15000.zip"))
rows = []
for line in io.TextIOWrapper(z.open("cities15000.txt"), encoding="utf-8"):
    f = line.rstrip("\n").split("\t")
    name, lat, lon, cc, a1, pop = f[1], float(f[4]), float(f[5]), f[8], f[10], int(f[14] or 0)
    if "\t" in name or not name:
        continue
    rows.append((name, cc, regions.get(f"{cc}.{a1}", ""), round(lat, 4), round(lon, 4), pop))
rows.sort(key=lambda r: (r[3], r[4]))
with open(os.path.join(out, "cities.tsv"), "w", encoding="utf-8") as o:
    for r in rows:
        o.write("\t".join(str(x) for x in r) + "\n")

# Shapes: polygon rings from a shapefile in a zip.
def rings(zipname, shpname):
    data = zipfile.ZipFile(os.path.join(src, zipname)).read(shpname)
    at, result = 100, []
    while at + 8 <= len(data):
        length = struct.unpack_from(">i", data, at + 4)[0] * 2
        rec = data[at + 8: at + 8 + length]
        at += 8 + length
        if len(rec) < 44 or struct.unpack_from("<i", rec, 0)[0] not in (3, 5):
            continue
        parts, points = struct.unpack_from("<ii", rec, 36)
        starts = list(struct.unpack_from(f"<{parts}i", rec, 44)) + [points]
        base = 44 + 4 * parts
        pts = [struct.unpack_from("<dd", rec, base + 16 * i) for i in range(points)]
        for a, b in zip(starts, starts[1:]):
            result.append(pts[a:b])
    return result

def simplify(pts, tol):
    # Douglas-Peucker, iterative.
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        (x1, y1), (x2, y2) = pts[a], pts[b]
        dx, dy = x2 - x1, y2 - y1
        n = (dx * dx + dy * dy) ** 0.5
        best, at = -1.0, -1
        for i in range(a + 1, b):
            x, y = pts[i]
            d = abs(dy * (x - x1) - dx * (y - y1)) / n if n else ((x - x1) ** 2 + (y - y1) ** 2) ** 0.5
            if d > best:
                best, at = d, i
        if best > tol and at > 0:
            keep[at] = True
            stack += [(a, at), (at, b)]
    return [p for p, k in zip(pts, keep) if k]

def write(name, rs, tol):
    total = 0
    with open(os.path.join(out, name), "wb") as o:
        o.write(b"DAMAP1\0\0")
        for r in rs:
            r = simplify(r, tol)
            if len(r) < 2:
                continue
            o.write(struct.pack("<I", len(r)))
            for x, y in r:
                o.write(struct.pack("<hh", round(x * 100), round(y * 100)))
            total += len(r)
    print(name, total, "points", os.path.getsize(os.path.join(out, name)), "bytes")

write("land.bin", rings("ne_50m_land.zip", "ne_50m_land.shp"), 0.02)
write("borders.bin", rings("ne_110m_admin_0_countries.zip", "ne_110m_admin_0_countries.shp"), 0.05)
print(len(rows), "cities,", len(countries), "countries")
