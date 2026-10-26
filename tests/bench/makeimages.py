#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Danny S
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the recovery benchmark's test drives (run as root, once).

    sudo tests/bench/makeimages.py <work folder>

In <work folder>/corpus: about 150 files of the kinds people lose (photos, films, music,
Office files, PDFs, archives, databases, text, tiny files), made here from nothing, so they're
ours. In <work folder>/images, for NTFS, FAT32, exFAT and ext4:

  <fs>-deleted.img    folders of files copied in, the drive made fragmented on purpose, about
                      half the files deleted, then new files written over some of the freed space
  <fs>-formatted.img  files copied in, then quick-formatted and a couple of new files written

Everything goes through Linux's own drivers (loop device and mount), so the deletions look the way
they really do. Each image gets <image>.json: every file that was on it, where, its SHA-256, the
SHA-256 of each 4 KiB block of it, and what happened to it (live, deleted, formatted). Images are
kept: run it again with --remake to make new ones (which makes earlier results incomparable).
"""

import hashlib
import json
import os
import random
import shutil
import sqlite3
import subprocess
import sys
import zipfile

SIZE_MB = 512
BLOCK = 4096


def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)


def ffmpeg(*args):
    run('ffmpeg', '-v', 'error', '-y', *args)


# ---- the files ---------------------------------------------------------------------------

def words(rng, n):
    vocab = ('drive disk photo family holiday invoice report summer winter garden house car beach '
             'mountain river budget meeting project notes letter recipe school music film backup '
             'account order delivery payment january february march april may june july').split()
    return ' '.join(rng.choice(vocab) for _ in range(n))


def office(path, kind, rng, pictures):
    """A real Word, Excel or PowerPoint file: the smallest set of parts Office opens."""
    text = words(rng, rng.randint(200, 20000))
    ct = {'docx': 'application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml',
          'xlsx': 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml',
          'pptx': 'application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml'}[kind]
    main = {'docx': 'word/document.xml', 'xlsx': 'xl/workbook.xml', 'pptx': 'ppt/presentation.xml'}[kind]
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('[Content_Types].xml',
                   '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                   '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                   '<Default Extension="xml" ContentType="application/xml"/><Default Extension="jpeg" ContentType="image/jpeg"/>'
                   f'<Override PartName="/{main}" ContentType="{ct}"/></Types>')
        z.writestr('_rels/.rels',
                   '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" '
                   f'Target="{main}"/></Relationships>')
        if kind == 'docx':
            z.writestr(main, '<?xml version="1.0" encoding="UTF-8"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
                             f'<w:body><w:p><w:r><w:t>{text}</w:t></w:r></w:p></w:body></w:document>')
        elif kind == 'xlsx':
            z.writestr(main, '<?xml version="1.0" encoding="UTF-8"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">'
                             f'<sheets/><!-- {text} --></workbook>')
        else:
            z.writestr(main, '<?xml version="1.0" encoding="UTF-8"?><p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">'
                             f'<!-- {text} --></p:presentation>')
        if pictures and rng.random() < 0.5:
            z.write(rng.choice(pictures), {'docx': 'word', 'xlsx': 'xl', 'pptx': 'ppt'}[kind] + '/media/image1.jpeg')


def pdf(path, rng):
    """A small PDF with text, written by hand (a correct xref table and trailer)."""
    lines = [words(rng, 12) for _ in range(rng.randint(5, 400))]
    stream = 'BT /F1 11 Tf 50 780 Td 14 TL ' + ' '.join(f'({l}) \'' for l in lines) + ' ET'
    objects = ['<< /Type /Catalog /Pages 2 0 R >>',
               '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
               '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>',
               f'<< /Length {len(stream)} >>\nstream\n{stream}\nendstream',
               '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']
    out = bytearray(b'%PDF-1.4\n')
    offsets = []
    for i, body in enumerate(objects, 1):
        offsets.append(len(out))
        out += f'{i} 0 obj\n{body}\nendobj\n'.encode()
    xref = len(out)
    out += f'xref\n0 {len(objects) + 1}\n0000000000 65535 f \n'.encode()
    for o in offsets:
        out += f'{o:010d} 00000 n \n'.encode()
    out += f'trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n'.encode()
    with open(path, 'wb') as f:
        f.write(out)


def make_corpus(folder):
    rng = random.Random(20261026)
    os.makedirs(folder, exist_ok=True)
    files = []  # (relative folder, name)

    def add(sub, name):
        os.makedirs(os.path.join(folder, sub), exist_ok=True)
        files.append((sub, name))
        return os.path.join(folder, sub, name)

    sources = ['testsrc2', 'mandelbrot', 'cellauto', 'life', 'rgbtestsrc', 'smptehdbars']
    sizes = [(320, 240), (640, 480), (1024, 768), (1600, 1200), (2048, 1536), (3264, 2448), (4000, 3000)]
    pictures = []
    for i in range(40):
        w, h = sizes[i % len(sizes)]
        src = sources[i % len(sources)]
        p = add('Photos/2024' if i % 2 else 'Photos/2025', f'IMG_{2000 + i}.jpg')
        ffmpeg('-f', 'lavfi', '-i', f'{src}=size={w}x{h}', '-frames:v', '1', '-q:v', str(2 + i % 6), p)
        pictures.append(p)
    for i in range(15):
        w, h = sizes[i % 5]
        ffmpeg('-f', 'lavfi', '-i', f'{sources[(i + 2) % len(sources)]}=size={w}x{h}', '-frames:v', '1', add('Pictures', f'screenshot {i + 1}.png'))
    for i in range(5):
        ffmpeg('-f', 'lavfi', '-i', 'testsrc2=size=320x240:rate=10', '-t', '2', add('Pictures', f'animation {i + 1}.gif'))
    for i in range(10):
        ffmpeg('-f', 'lavfi', '-i', f'{sources[i % len(sources)]}=size=1280x720:rate=30', '-t', str(2 + i), '-c:v', 'libx264',
               '-pix_fmt', 'yuv420p', '-preset', 'veryfast', add('Videos', f'VID_{3000 + i}.mp4' if i < 8 else f'clip {i}.mov'))
    for i in range(6):
        ffmpeg('-f', 'lavfi', '-i', f'sine=frequency={220 * (i + 1)}:duration={20 + 10 * i}', '-b:a', '192k', add('Music', f'track {i + 1:02d}.mp3'))
    for i in range(4):
        ffmpeg('-f', 'lavfi', '-i', f'anoisesrc=d={3 + i}:c=pink:seed={i}', add('Music', f'recording {i + 1}.wav'))
    for i in range(2):
        ffmpeg('-f', 'lavfi', '-i', f'anoisesrc=d=8:c=brown:seed={10 + i}', add('Music', f'master {i + 1}.flac'))
    for i in range(12):
        kind = ('docx', 'xlsx', 'pptx')[i % 3]
        office(add('Documents', f'{words(rng, 2).title()} {i + 1}.{kind}'), kind, rng, pictures[:10])
    for i in range(10):
        pdf(add('Documents', f'statement {i + 1}.pdf'), rng)
    for i in range(3):
        run('magick', pictures[i * 3], add('Documents', f'scan {i + 1}.pdf'))
    for i in range(5):
        p = add('Archive', f'backup {i + 1}.zip')
        with zipfile.ZipFile(p, 'w', zipfile.ZIP_DEFLATED) as z:
            for j in range(3 + i):
                z.write(pictures[(i * 5 + j) % len(pictures)], f'photo{j}.jpg')
                z.writestr(f'notes{j}.txt', words(rng, 500))
    for i in range(5):
        db = sqlite3.connect(add('Archive', f'library {i + 1}.sqlite'))
        db.execute('create table books (id integer primary key, title text, notes text)')
        db.executemany('insert into books (title, notes) values (?, ?)', [(words(rng, 4), words(rng, 60)) for _ in range(200 * (i + 1))])
        db.commit()
        db.close()
    for i in range(8):
        with open(add('Documents', f'notes {i + 1}.txt'), 'w') as f:
            f.write('\n'.join(words(rng, 15) for _ in range(rng.randint(20, 3000))))
    for i in range(10):  # small enough for NTFS to keep inside its records
        with open(add('Documents/small', f'todo {i + 1}.txt'), 'w') as f:
            f.write(words(rng, rng.randint(10, 80)))
    for i in range(3):  # big ones, to be spread over the holes
        ffmpeg('-f', 'lavfi', '-i', 'testsrc2=size=1920x1080:rate=30', '-t', str(20 + 5 * i), '-c:v', 'libx264', '-preset', 'veryfast',
               '-pix_fmt', 'yuv420p', '-b:v', '3M', add('Videos/big', f'wedding part {i + 1}.mp4'))
    return files


# ---- the drives ------------------------------------------------------------------------------

def fingerprint(path):
    blocks, whole = [], hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(BLOCK):
            whole.update(chunk)
            blocks.append(hashlib.sha256(chunk).hexdigest()[:16])
    return whole.hexdigest(), blocks


MKFS = {
    # ntfs-3g, not the kernel's ntfs3: ntfs3 wipes a deleted file's name out of its record, and
    # Windows (like ntfs-3g) leaves it there, which is what recovery tools find names from.
    'ntfs': (['mkntfs', '-q', '-F', '-f', '-L', 'BENCH'], 'ntfs-3g'),
    'fat32': (['mkfs.fat', '-F', '32', '-n', 'BENCH'], 'vfat'),
    'exfat': (['mkfs.exfat', '-L', 'BENCH'], 'exfat'),
    # nodiscard: on an image file mke2fs would otherwise blank the whole drive (like TRIM on an
    # SSD); a quick format of a hard drive leaves what was there.
    'ext4': (['mkfs.ext4', '-q', '-F', '-E', 'nodiscard', '-L', 'BENCH'], 'ext4'),
}


class Mounted:
    def __init__(self, image, fs):
        self.image, self.fs = image, fs

    def __enter__(self):
        self.loop = run('losetup', '-f', '--show', self.image).stdout.decode().strip()
        self.dir = self.image + '.mnt'
        os.makedirs(self.dir, exist_ok=True)
        run('mount', '-t', MKFS[self.fs][1], self.loop, self.dir)
        return self.dir

    def __exit__(self, *exc):
        run('sync')
        run('umount', self.dir)
        run('losetup', '-d', self.loop)
        os.rmdir(self.dir)


def mkfs(image, fs):
    with open(image, 'wb') as f:
        f.truncate(SIZE_MB * 1024 * 1024)
    run(*MKFS[fs][0], image)


def copy_in(corpus, files, top, entries, state):
    for sub, name in files:
        os.makedirs(os.path.join(top, sub), exist_ok=True)
        src = os.path.join(corpus, sub, name)
        shutil.copyfile(src, os.path.join(top, sub, name))
        whole, blocks = fingerprint(src)
        entries.append({'folder': sub, 'name': name, 'size': os.path.getsize(src), 'sha256': whole, 'blocks': blocks, 'state': state})


def other(path, others):
    """A file that isn't one of the test files (a filler, something written later): recovering it isn't junk."""
    whole, blocks = fingerprint(path)
    others.append({'sha256': whole, 'blocks': blocks})


def deleted_image(image, fs, corpus, files, others):
    rng = random.Random(f'{fs}-deleted')
    mkfs(image, fs)
    entries = []
    with Mounted(image, fs) as top:
        # Holes: 2 MB fillers over most of the drive, every other one deleted, so what's copied
        # in next lands in pieces.
        os.makedirs(os.path.join(top, 'filler'))
        fillers = []
        for i in range(int(SIZE_MB * 0.45 / 2)):
            path = os.path.join(top, 'filler', f'f{i:04d}.bin')
            with open(path, 'wb') as f:
                f.write(rng.randbytes(2 * 1024 * 1024))
            fillers.append(path)
            other(path, others)
        run('sync')
        for path in fillers[::2]:
            os.remove(path)
        run('sync')
        copy_in(corpus, files, top, entries, 'live')
        run('sync')
        # About half of each folder deleted, the big films always.
        for e in entries:
            if e['folder'] == 'Videos/big' or rng.random() < 0.5:
                os.remove(os.path.join(top, e['folder'], e['name']))
                e['state'] = 'deleted'
        run('sync')
        # New files over some of the freed space.
        os.makedirs(os.path.join(top, 'New'))
        for i in range(12):
            path = os.path.join(top, 'New', f'new{i}.bin')
            with open(path, 'wb') as f:
                f.write(rng.randbytes(rng.randint(64, 3000) * 1024))
            other(path, others)
    return entries


def formatted_image(image, fs, corpus, files, others):
    rng = random.Random(f'{fs}-formatted')
    mkfs(image, fs)
    entries = []
    with Mounted(image, fs) as top:
        copy_in(corpus, files, top, entries, 'formatted')
    run(*MKFS[fs][0], image)  # the quick format
    with Mounted(image, fs) as top:
        for i in range(2):
            path = os.path.join(top, f'after{i}.bin')
            with open(path, 'wb') as f:
                f.write(rng.randbytes(512 * 1024))
            other(path, others)
    return entries


def main():
    if len(sys.argv) < 2 or os.geteuid() != 0:
        sys.exit('usage (as root): makeimages.py <work folder> [--remake]')
    work = sys.argv[1]
    corpus, images = os.path.join(work, 'corpus'), os.path.join(work, 'images')
    remake = '--remake' in sys.argv
    if remake:
        shutil.rmtree(corpus, ignore_errors=True)
        shutil.rmtree(images, ignore_errors=True)
    os.makedirs(images, exist_ok=True)
    listing = os.path.join(corpus, 'files.json')
    if os.path.exists(listing):
        files = [tuple(f) for f in json.load(open(listing))]
    else:
        files = make_corpus(corpus)
        json.dump(files, open(listing, 'w'))
    for fs in MKFS:
        for what, make in (('deleted', deleted_image), ('formatted', formatted_image)):
            image = os.path.join(images, f'{fs}-{what}.img')
            if os.path.exists(image + '.json'):
                continue
            print(f'{fs}-{what}...', flush=True)
            others = []
            entries = make(image, fs, corpus, files, others)
            json.dump({'image': os.path.basename(image), 'fs': fs, 'kind': what, 'files': entries, 'others': others},
                      open(image + '.json', 'w'))
    owner = os.stat(work)
    for root, dirs, names in os.walk(work):
        for n in dirs + names:
            os.chown(os.path.join(root, n), owner.st_uid, owner.st_gid)


if __name__ == '__main__':
    main()
