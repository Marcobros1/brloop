"""Extract readable UI strings from the FLSM executable (text resources only).

Method: scan for printable ASCII and UTF-16LE sequences. No disassembly,
no IL decoding - only user-visible text, to document offered options.
"""
import re
import sys

PATH = r"<personal-path> Lap Stream Maker.exe"
data = open(PATH, "rb").read()
print("exe size:", len(data))

ascii_strings = [m.group().decode("ascii") for m in re.finditer(rb"[\x20-\x7e]{5,}", data)]
utf16_strings = [m.group().decode("utf-16-le", errors="replace")
                 for m in re.finditer(rb"(?:[\x20-\x7e]\x00){5,}", data)]

KEY = re.compile(r"(?i)(speed|lap|pitch|tempo|brstm|bcstm|bfstm|percent|%|hz|rate|loop|convert|output|save|open|drag|drop|version|music|slow|fast|increase|decrease|semitone)")

print("\n== UTF-16 strings matching keywords ==")
seen = set()
for s in utf16_strings:
    if KEY.search(s) and s not in seen:
        seen.add(s)
        print(" ", s)

print("\n== ASCII strings matching keywords ==")
seen = set()
for s in ascii_strings:
    if KEY.search(s) and s not in seen:
        seen.add(s)
        print(" ", s)

print("\n== UTF-16 short UI-like strings (first 120, len 5..60) ==")
count = 0
for s in utf16_strings:
    if 5 <= len(s) <= 60 and not s.startswith("http") and any(c.isalpha() for c in s):
        print(" ", s)
        count += 1
        if count >= 120:
            break
