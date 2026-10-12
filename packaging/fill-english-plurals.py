#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Danny S
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fills the English plural forms in i18n/diskanvil_en.ts: "%n sector(s)" becomes
# "%n sector" / "%n sectors". Run after `cmake --build build --target update_translations`.
import html
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "i18n/diskanvil_en.ts"
text = open(path, encoding="utf-8").read()

def fill(match):
    block = match.group(0)
    source = html.unescape(re.search(r"<source>(.*?)</source>", block, re.S).group(1))
    one = source.replace("(s)", "")
    many = source.replace("(s)", "s")
    forms = "".join(f"\n            <numerusform>{html.escape(f, quote=False)}</numerusform>" for f in (one, many))
    return re.sub(r"<translation[^>]*>.*?</translation>", f"<translation>{forms}\n        </translation>", block, flags=re.S)

text = re.sub(r'<message numerus="yes">.*?</message>', fill, text, flags=re.S)
open(path, "w", encoding="utf-8").write(text)
