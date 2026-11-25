#!/usr/bin/env python3
"""Makes docs/Chord-Stage-Help.pdf from HELP.md, so the PDF always matches the Help page.

    python3 docs/make_help_pdf.py          (from the project folder)

It turns HELP.md into print-styled HTML (headings, paragraphs, lists, tables, images, **bold**
and `code`, which is all HELP.md uses) and prints it with Google Chrome in headless mode.
Standard library only; needs Google Chrome (or set CHROME to a Chromium-based browser).
"""
import html
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
CHROME = os.environ.get("CHROME") or "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

CSS = """
@page { size: A4; margin: 16mm 15mm 18mm; }
body { font: 10.5pt/1.45 -apple-system, "Helvetica Neue", Arial, sans-serif; color: #1d1d1f; }
h1 { font-size: 24pt; margin: 0 0 4pt; color: #5b2fd1; }
h2 { font-size: 15pt; margin: 18pt 0 6pt; padding-bottom: 3pt; border-bottom: 2px solid #9b63ff; break-after: avoid; }
p { margin: 5pt 0; }
ul, ol { margin: 4pt 0 6pt; padding-left: 18pt; }
li { margin: 2pt 0; }
table { border-collapse: collapse; width: 100%; margin: 6pt 0 8pt; font-size: 9.5pt; break-inside: auto; }
tr { break-inside: avoid; }
th, td { border: 1px solid #d5d5da; padding: 4pt 6pt; text-align: left; vertical-align: top; }
th { background: #f1ecff; }
code { font: 9pt Menlo, monospace; background: #f3f3f5; padding: 0 3px; border-radius: 3px; }
img { display: block; width: 84%; margin: 4pt auto 6pt; border-radius: 6px; break-inside: avoid; }
.page-break { break-before: page; }
"""


def inline(text):
    out = html.escape(text, quote=False)
    out = re.sub(r"`([^`]+)`", r"<code>\1</code>", out)
    out = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", out)
    return out


def to_html(md):
    lines = md.splitlines()
    out, i, para = [], 0, []

    def flush():
        if para:
            out.append("<p>%s</p>" % inline(" ".join(para)))
            para.clear()

    while i < len(lines):
        line = lines[i]
        if not line.strip():
            flush(); i += 1; continue
        m = re.match(r"^(#{1,3}) (.*)", line)
        if m:
            flush()
            level = len(m.group(1))
            # The PLAY page and Progressions sections start on a new page
            cls = ' class="page-break"' if level == 2 and re.match(r"^(3|5)\. ", m.group(2)) else ""
            out.append("<h%d%s>%s</h%d>" % (level, cls, inline(m.group(2)), level))
            i += 1; continue
        m = re.match(r"^!\[([^\]]*)\]\(([^)]+)\)", line)
        if m:
            flush()
            src = "file://" + os.path.join(PROJECT, m.group(2))
            out.append('<img alt="%s" src="%s">' % (html.escape(m.group(1)), html.escape(src)))
            i += 1; continue
        if line.startswith("|"):
            flush()
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                if not all(re.match(r"^:?-{3,}:?$", c) for c in cells):
                    rows.append(cells)
                i += 1
            t = ["<table><thead><tr>%s</tr></thead><tbody>" % "".join("<th>%s</th>" % inline(c) for c in rows[0])]
            t += ["<tr>%s</tr>" % "".join("<td>%s</td>" % inline(c) for c in r) for r in rows[1:]]
            out.append("".join(t) + "</tbody></table>")
            continue
        m = re.match(r"^(\d+\.|-) (.*)", line)
        if m:
            flush()
            tag = "ol" if m.group(1)[0].isdigit() else "ul"
            items = []
            while i < len(lines):
                m = re.match(r"^(\d+\.|-) (.*)", lines[i])
                if not m:
                    break
                items.append("<li>%s</li>" % inline(m.group(2)))
                i += 1
            out.append("<%s>%s</%s>" % (tag, "".join(items), tag))
            continue
        para.append(line.strip())
        i += 1
    flush()
    return "\n".join(out)


def main():
    md = open(os.path.join(PROJECT, "HELP.md"), encoding="utf-8").read()
    # The PDF is the PDF: drop the line that points to it
    md = "\n".join(l for l in md.splitlines() if "Chord-Stage-Help.pdf" not in l)
    page = ('<!doctype html><html><head><meta charset="utf-8"><title>Chord Stage Help</title>'
            "<style>%s</style></head><body>%s</body></html>" % (CSS, to_html(md)))
    out_pdf = os.path.join(HERE, "Chord-Stage-Help.pdf")
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "help.html")
        open(src, "w", encoding="utf-8").write(page)
        subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
                        "--allow-file-access-from-files", "--user-data-dir=" + os.path.join(tmp, "profile"),
                        "--print-to-pdf=" + out_pdf, "file://" + src],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    print("wrote", out_pdf)


if __name__ == "__main__":
    sys.exit(main())
