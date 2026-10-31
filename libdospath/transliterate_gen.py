#!/usr/bin/env python3
# Generates the translitTable[] rows for transliterate.cpp into table.inc - paste them into the table there.
# Latin chars are made by Unicode NFKD decomposition with dropped accents (+ overrides below), Greek and Cyrillic by the tables below.
import unicodedata
OK = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789'-")
over = {
 'ß':'ss','ẞ':'SS','Æ':'AE','æ':'ae','Œ':'OE','œ':'oe','Ø':'O','ø':'o','Đ':'D','đ':'d','Ð':'D','ð':'d',
 'Þ':'TH','þ':'th','Ł':'L','ł':'l','ı':'i','ĸ':'k','Ŋ':'N','ŋ':'n','Ŧ':'T','ŧ':'t','Ħ':'H','ħ':'h',
 'Ŀ':'L','ŀ':'l','ŉ':"'n",'ſ':'s','Ə':'E','ə':'e','Ɛ':'E','ɛ':'e','ƒ':'f','×':'x','µ':'u',' ':' ',
 '‐':'-','‑':'-','‒':'-','–':'-','—':'-','―':'-','‘':"'",'’':"'",'‚':"'",'‛':"'",'´':"'",'`':"'",
}
greek = dict(zip("ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩ",
 "A B G D E Z I TH I K L M N X O P R S T Y F CH PS O".split()))
cyr = {'А':'A','Б':'B','В':'V','Г':'G','Д':'D','Е':'E','Ж':'ZH','З':'Z','И':'I','Й':'Y','К':'K','Л':'L',
 'М':'M','Н':'N','О':'O','П':'P','Р':'R','С':'S','Т':'T','У':'U','Ф':'F','Х':'KH','Ц':'TS','Ч':'CH',
 'Ш':'SH','Щ':'SHCH','Ъ':'','Ы':'Y','Ь':'','Э':'E','Ю':'YU','Я':'YA','Ѐ':'E','Ё':'E','Ђ':'DJ','Ѓ':'G',
 'Є':'YE','Ѕ':'DZ','І':'I','Ї':'YI','Ј':'J','Љ':'LJ','Њ':'NJ','Ћ':'C','Ќ':'K','Ѝ':'I','Ў':'U','Џ':'DZ',
 'Ґ':'G'}
tab = {}
def add(cp, s):
    if s is None: return
    tab[cp] = s
for cp in list(range(0xA0,0x250)) + list(range(0x1E00,0x1F00)) + list(range(0x2010,0x2020)):
    c = chr(cp)
    if c in over: add(cp, over[c]); continue
    d = unicodedata.normalize('NFKD', c)
    d = ''.join(x for x in d if not unicodedata.combining(x))
    if d and all(x in OK for x in d) and len(d) <= 3: add(cp, d)
for cp in range(0x370,0x400):
    c = chr(cp); d = unicodedata.normalize('NFD', c)[0].upper()
    if d in greek: add(cp, greek[d] if c.isupper() or not c.isalpha() else greek[d].lower())
for cp in range(0x400,0x460):
    c = chr(cp); u = c.upper()
    if u in cyr: add(cp, cyr[u] if c.isupper() else cyr[u].lower())
for cp in range(0x490,0x492):
    c = chr(cp); add(cp, 'G' if c.isupper() else 'g')
add(0x2116, 'N')   # numero sign
assert all(len(v) <= 4 for v in tab.values())
items = sorted(tab.items())
out = []
line = '    '
for cp, s in items:
    s2 = s.replace("'", "\\'")
    ent = "{0x%04X,\"%s\"}, " % (cp, s.replace('"','\\"'))
    if len(line) + len(ent) > 120:
        out.append(line.rstrip()); line = '    '
    line += ent
out.append(line.rstrip())
open('table.inc','w').write('\n'.join(out) + '\n')
print(len(items))
