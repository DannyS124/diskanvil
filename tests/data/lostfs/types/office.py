# Writes the flat OpenDocument files LibreOffice turns into .doc, .xls and .ppt for the tests
# (see make.sh), and the Outlook email (.msg) LibreOffice can't make, with a small compound file
# writer of our own. Run in the container make.sh starts:
#   python3 -I office.py <folder>
import datetime
import struct
import sys

out = sys.argv[1]

NS = ('xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
      'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
      'xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" '
      'xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0" '
      'xmlns:presentation="urn:oasis:names:tc:opendocument:xmlns:presentation:1.0" '
      'xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
      'xmlns:meta="urn:oasis:names:tc:opendocument:xmlns:meta:1.0" '
      'xmlns:dc="http://purl.org/dc/elements/1.1/" office:version="1.3"')

META = ('<office:meta><dc:title>{title}</dc:title><meta:initial-creator>Sam Tester</meta:initial-creator>'
        '<dc:creator>Sam Tester</dc:creator><meta:creation-date>2024-07-14T13:22:05</meta:creation-date>'
        '<dc:subject>Summer trip</dc:subject></office:meta>')


def write(name, text):
    with open(f"{out}/{name}", "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n' + text)


# A letter with more than one piece of text, and words that aren't all plain ASCII.
paragraphs = ["Pack the blue tent and the camera.",
              "The ferry leaves at 07:45 from pier 3, so don't be late.",
              "Unicode check: café, naïve, Straße, 東京."]
paragraphs += [f"Line {i:03d} of the packing list." for i in range(1, 121)]
write("notes.fodt", f'<office:document {NS} office:mimetype="application/vnd.oasis.opendocument.text">'
      + META.format(title="Holiday plans")
      + '<office:body><office:text>' + ''.join(f'<text:p>{p}</text:p>' for p in paragraphs)
      + '</office:text></office:body></office:document>')


def cell(v):
    if isinstance(v, str):
        return f'<table:table-cell office:value-type="string"><text:p>{v}</text:p></table:table-cell>'
    return f'<table:table-cell office:value-type="float" office:value="{v}"><text:p>{v}</text:p></table:table-cell>'


def rows(data):
    return ''.join('<table:table-row>' + ''.join(cell(v) for v in r) + '</table:table-row>' for r in data)


# Two sheets; the second has enough different words that Excel's table of them runs over into
# CONTINUE records, with words split across them.
groceries = [["Item", "Cost"], ["Bread", 2.5], ["Milk", 1.25], ["Café crème", 3.75]]
total = ('<table:table-row><table:table-cell office:value-type="string"><text:p>Total</text:p></table:table-cell>'
         '<table:table-cell table:formula="of:=SUM([.B2:.B4])" office:value-type="float" office:value="7.5">'
         '<text:p>7.5</text:p></table:table-cell></table:table-row>')
places = [[f"Place number {i:04d} on the long road north", i * 7] for i in range(1, 801)]
write("budget.fods", f'<office:document {NS} office:mimetype="application/vnd.oasis.opendocument.spreadsheet">'
      + META.format(title="Budget")
      + '<office:body><office:spreadsheet>'
      + '<table:table table:name="Groceries">' + rows(groceries) + total + '</table:table>'
      + '<table:table table:name="Road">' + rows(places) + '</table:table>'
      + '</office:spreadsheet></office:body></office:document>')


def frame(y, text):
    return (f'<draw:frame svg:x="2cm" svg:y="{y}cm" svg:width="20cm" svg:height="3cm">'
            f'<draw:text-box><text:p>{text}</text:p></draw:text-box></draw:frame>')


write("slides.fodp", f'<office:document {NS} office:mimetype="application/vnd.oasis.opendocument.presentation">'
      + META.format(title="Trip slides")
      + '<office:body><office:presentation>'
      + '<draw:page draw:name="page1">' + frame(2, "Summer trip") + frame(6, "Day one: the lake") + '</draw:page>'
      + '<draw:page draw:name="page2">' + frame(2, "Day two: the mountains") + '</draw:page>'
      + '</office:presentation></office:body></office:document>')


# --- A compound file (MS-CFB, version 3: 512-byte sectors), from (path, bytes) pairs; a path
# with a "/" is in a storage. Streams under 4096 bytes go in the mini stream, as they must.

END, FREE, FATSECT = 0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFD


def parent_of(path):
    return path.rsplit("/", 1)[0] if "/" in path else ""


def compound(streams):
    storages = sorted({p.rsplit("/", 1)[0] for p, _ in streams if "/" in p})
    entries = [{"name": "Root Entry", "type": 5, "path": ""}]
    entries += [{"name": s.rsplit("/", 1)[-1], "type": 1, "path": s} for s in storages]
    entries += [{"name": p.rsplit("/", 1)[-1], "type": 2, "path": p, "data": d} for p, d in streams]
    # Each storage's children, in the order CFB compares names (length first, then upper case),
    # one after another as right siblings.
    for e in entries:
        e.update(left=FREE, right=FREE, child=FREE, start=END, size=0)
    for parent in entries:
        if parent["type"] == 2:
            continue
        kids = [i for i, e in enumerate(entries) if e["path"] and parent_of(e["path"]) == parent["path"]]
        kids.sort(key=lambda i: (len(entries[i]["name"]), entries[i]["name"].upper()))
        if kids:
            parent["child"] = kids[0]
            for a, b in zip(kids, kids[1:]):
                entries[a]["right"] = b
    mini = bytearray()
    minifat = []
    big = []
    for e in entries:
        if e["type"] != 2:
            continue
        d = e["data"]
        e["size"] = len(d)
        if len(d) < 4096:
            if not d:
                continue
            e["start"] = len(mini) // 64
            n = (len(d) + 63) // 64
            minifat += [e["start"] + k + 1 for k in range(n - 1)] + [END]
            mini += d + b"\0" * (n * 64 - len(d))
        else:
            big.append(e)
    entries[0]["size"] = len(mini)

    def sectors(n):
        return (n + 511) // 512

    dir_sectors = sectors(len(entries) * 128)
    minifat_sectors = sectors(len(minifat) * 4)
    mini_sectors = sectors(len(mini))
    big_sectors = sum(sectors(len(e["data"])) for e in big)
    fat_sectors = 1
    while (fat_sectors * 128) < fat_sectors + dir_sectors + minifat_sectors + mini_sectors + big_sectors:
        fat_sectors += 1
    fat = [FATSECT] * fat_sectors

    def chain(n):
        start = len(fat)
        fat.extend([start + k + 1 for k in range(n - 1)] + [END])
        return start

    dir_start = chain(dir_sectors)
    minifat_start = chain(minifat_sectors) if minifat_sectors else END
    entries[0]["start"] = chain(mini_sectors) if mini_sectors else END
    for e in big:
        e["start"] = chain(sectors(len(e["data"])))
    fat += [FREE] * (fat_sectors * 128 - len(fat))
    head = b"\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1" + b"\0" * 16
    head += struct.pack("<HHHHH6sIIIIIIIII", 0x3E, 3, 0xFFFE, 9, 6, b"\0" * 6, 0, fat_sectors, dir_start, 0, 4096,
                        minifat_start, minifat_sectors, END, 0)
    head += b"".join(struct.pack("<I", k) for k in range(fat_sectors)) + struct.pack("<I", FREE) * (109 - fat_sectors)
    body = b"".join(struct.pack("<I", v) for v in fat)
    d = bytearray()
    for e in entries:
        name = e["name"].encode("utf-16-le") + b"\0\0"
        d += name + b"\0" * (64 - len(name))
        d += struct.pack("<HBBIII16sIQQIQ", len(name), e["type"], 1, e["left"], e["right"], e["child"], b"\0" * 16, 0, 0, 0,
                         e["start"], e["size"])
    d += b"\0" * (dir_sectors * 512 - len(d))
    body += bytes(d)
    mf = b"".join(struct.pack("<I", v) for v in minifat)
    body += mf + struct.pack("<I", FREE) * ((minifat_sectors * 512 - len(mf)) // 4)
    body += bytes(mini) + b"\0" * (mini_sectors * 512 - len(mini))
    for e in big:
        body += e["data"] + b"\0" * (sectors(len(e["data"])) * 512 - len(e["data"]))
    return head + body


def u16(text):
    return text.encode("utf-16-le")


def filetime(t):
    return int((t - datetime.datetime(1601, 1, 1, tzinfo=datetime.timezone.utc)).total_seconds() * 10_000_000)


sent = datetime.datetime(2024, 7, 14, 13, 22, 5, tzinfo=datetime.timezone.utc)
props = b"\0" * 32 + struct.pack("<IIQ", 0x00390040, 6, filetime(sent))
body = "Hi Alex,\r\n\r\nThe ferry tickets are attached. " + "See you at pier 3. " * 260 + "\r\n\r\nSam"
ticket = b"%PDF-1.4\n% a made-up ticket\n" + b"0" * 6000
mail = [
    ("__properties_version1.0", props),
    ("__substg1.0_001A001F", u16("IPM.Note")),
    ("__substg1.0_0037001F", u16("Ferry tickets")),
    ("__substg1.0_0C1A001F", u16("Sam Tester")),
    ("__substg1.0_5D01001F", u16("sam@example.com")),
    ("__substg1.0_0E04001F", u16("Alex Example")),
    ("__substg1.0_1000001F", u16(body)),
    ("__recip_version1.0_#00000000/__properties_version1.0", b"\0" * 8),
    ("__recip_version1.0_#00000000/__substg1.0_3001001F", u16("Alex Example")),
    ("__attach_version1.0_#00000000/__properties_version1.0", b"\0" * 8),
    ("__attach_version1.0_#00000000/__substg1.0_3707001F", u16("tickets.pdf")),
    ("__attach_version1.0_#00000000/__substg1.0_37010102", ticket),
]
with open(f"{out}/ferry.msg", "wb") as f:
    f.write(compound(mail))

# Windows' thumbnail caches (run here, it needs PIL): a thumbcache_256.db as Windows 10 writes them
# (the "CMMM" header, then entries of a hash, sizes, the picture's width and height, checksums, an
# identifier and the picture), with an empty entry among them as Windows leaves; and an XP Thumbs.db,
# a compound file with a Catalog naming each photo and a stream per picture.
if len(sys.argv) > 2 and sys.argv[2] == "thumbs":
    import io
    from PIL import Image, ImageDraw

    def thumb(colour, kind, size=(96, 64)):
        img = Image.new("RGB", size, colour)
        ImageDraw.Draw(img).ellipse((10, 10, 50, 50), fill=(255, 255, 255))
        out = io.BytesIO()
        img.save(out, kind)
        return out.getvalue()

    pictures = [thumb((255, 51, 85), "JPEG"), thumb((43, 238, 138), "PNG"), b"", thumb((31, 111, 255), "JPEG")]
    body = b""
    first = 24
    for i, data in enumerate(pictures):
        ident = ("%016x" % (0x1234abcd0000 + i)).encode("utf-16-le")
        header = 56 + len(ident)
        padding = (-(header + len(data))) % 8
        entry = b"CMMM" + struct.pack("<IQIIIIIIQQ", header + padding + len(data), 0x9e1f + i, len(ident), padding, len(data),
                                      96 if data else 0, 64 if data else 0, 0, 0, 0)
        body += entry + ident + b"\0" * padding + data
    with open(f"{out}/thumbcache_256.db", "wb") as f:
        f.write(b"CMMM" + struct.pack("<IIIII", 0x20, 2, 0, first, first + len(body)) + body)

    names = {1: "beach.jpg", 2: "dog.png", 10: "party.jpg"}
    catalog = struct.pack("<HHIII", 16, 7, len(names), 96, 96)
    for number, name in names.items():
        text = name.encode("utf-16-le") + b"\0\0"
        text += b"\0" * ((-len(text)) % 4)
        catalog += struct.pack("<IIQ", 16 + len(text), number, filetime(sent)) + text
    streams = [("Catalog", catalog)]
    for number, (colour, kind) in zip(names, [((250, 200, 20), "JPEG"), ((120, 60, 200), "JPEG"), ((10, 160, 160), "JPEG")]):
        data = thumb(colour, kind)
        streams.append((str(number)[::-1], struct.pack("<III", 12, 1, len(data)) + data))
    with open(f"{out}/Thumbs.db", "wb") as f:
        f.write(compound(streams))
    sys.exit(0)

# The Word document again with a macro project in it, the way Word keeps one: the scanner and
# viewer must notice and not open it in Word. (Run after LibreOffice made notes.doc.)
if len(sys.argv) > 2 and sys.argv[2] == "macros":
    import olefile
    with olefile.OleFileIO(f"{out}/notes.doc") as doc:
        streams = [("/".join(p), doc.openstream(p).read()) for p in doc.listdir(streams=True, storages=False)]
    streams += [("Macros/PROJECT", b'ID="{00000000-0000-0000-0000-000000000000}"\r\nDocument=ThisDocument/&H00000000\r\n'),
                ("Macros/VBA/dir", b"\x01" + b"\0" * 40), ("Macros/VBA/ThisDocument", b"\0" * 600)]
    with open(f"{out}/macros.doc", "wb") as f:
        f.write(compound(streams))
