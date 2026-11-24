"""Builds samples/Notes-Demo.docx and samples/Notes-Demo.odt: footnotes and endnotes.

python-docx cannot write notes, so the .docx is built with it and then patched:
word/footnotes.xml and word/endnotes.xml are added the way Word writes them
(separator notes -1/0, w:footnoteRef in the body, FootnoteReference runs),
with their relationships and content types.

    uv run --with python-docx --with odfpy python tools/make_sample_notes.py
"""
import re
import zipfile
from xml.sax.saxutils import escape

from docx import Document

W = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"'
REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
CT = "application/vnd.openxmlformats-officedocument.wordprocessingml."

BODY = [
    ("h", "Fußnoten und Endnoten"),
    ("p", ["Der PowerBook Duo 230 kam 1992 heraus", ("f", "Vorgestellt im Oktober 1992, zusammen mit dem Duo 210."),
           " und wog 1,9 kg", ("f", "Ohne Dock. Mit dem MiniDock kommen Anschlüsse für Monitor, SCSI und ADB dazu."),
           "."]),
    ("p", ["Sein Prozessor ist ein 68030", ("f", "Motorola MC68030, 33 MHz, ohne FPU – die gibt es erst im Dock."),
           " mit 33 MHz. Quellen stehen am Ende", ("e", "Apple, Technical Introduction to the Macintosh Family."),
           ", wie in einer Hausarbeit", ("e", "Und eine zweite Endnote, römisch gezählt."), "."]),
    ("p", ["Ein Absatz ohne Anmerkung, damit man blättern kann."]),
]


def note_xml(kind, nid, text):
    ref = "FootnoteReference" if kind == "f" else "EndnoteReference"
    tag = "footnote" if kind == "f" else "endnote"
    return (f'<w:{tag} w:id="{nid}"><w:p><w:pPr><w:pStyle w:val="{"FootnoteText" if kind == "f" else "EndnoteText"}"/></w:pPr>'
            f'<w:r><w:rPr><w:rStyle w:val="{ref}"/></w:rPr><w:{tag}Ref/></w:r>'
            f'<w:r><w:t xml:space="preserve"> {escape(text)}</w:t></w:r></w:p></w:{tag}>')


def notes_part(kind, notes):
    tag = "footnote" if kind == "f" else "endnote"
    sep = (f'<w:{tag} w:type="separator" w:id="-1"><w:p><w:r><w:separator/></w:r></w:p></w:{tag}>'
           f'<w:{tag} w:type="continuationSeparator" w:id="0"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:{tag}>')
    return (f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<w:{tag}s {W}>{sep}'
            + "".join(note_xml(kind, i, t) for i, t in notes) + f"</w:{tag}s>")


def make_docx(path):
    d = Document()
    marks = []  # paragraph index -> list of runs
    for kind, content in BODY:
        if kind == "h":
            d.add_heading(content, 1)
        else:
            d.add_paragraph("@@%d@@" % len(marks))
            marks.append(content)
    d.save(path)

    z = zipfile.ZipFile(path)
    files = {n: z.read(n) for n in z.namelist()}
    z.close()
    doc = files["word/document.xml"].decode()
    notes = {"f": [], "e": []}
    for i, runs in enumerate(marks):
        xml = ""
        for r in runs:
            if isinstance(r, str):
                xml += f'<w:r><w:t xml:space="preserve">{escape(r)}</w:t></w:r>'
            else:
                k, text = r
                nid = len(notes[k]) + 1
                notes[k].append((nid, text))
                tag, style = ("footnote", "FootnoteReference") if k == "f" else ("endnote", "EndnoteReference")
                xml += (f'<w:r><w:rPr><w:rStyle w:val="{style}"/><w:vertAlign w:val="superscript"/></w:rPr>'
                        f'<w:{tag}Reference w:id="{nid}"/></w:r>')
        doc = re.sub(r"<w:r>(?:(?!<w:r>).)*?@@%d@@.*?</w:r>" % i, lambda m: xml, doc, count=1, flags=re.S)
    files["word/document.xml"] = doc.encode()
    files["word/footnotes.xml"] = notes_part("f", notes["f"]).encode()
    files["word/endnotes.xml"] = notes_part("e", notes["e"]).encode()
    rels = files["word/_rels/document.xml.rels"].decode()
    rels = rels.replace("</Relationships>",
                        f'<Relationship Id="rIdFn" Type="{REL}footnotes" Target="footnotes.xml"/>'
                        f'<Relationship Id="rIdEn" Type="{REL}endnotes" Target="endnotes.xml"/></Relationships>')
    files["word/_rels/document.xml.rels"] = rels.encode()
    ct = files["[Content_Types].xml"].decode()
    ct = ct.replace("</Types>",
                    f'<Override PartName="/word/footnotes.xml" ContentType="{CT}footnotes+xml"/>'
                    f'<Override PartName="/word/endnotes.xml" ContentType="{CT}endnotes+xml"/></Types>')
    files["[Content_Types].xml"] = ct.encode()
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for n, b in files.items():
            z.writestr(n, b)


def make_odt(path):
    from odf.opendocument import OpenDocumentText
    from odf.text import H, P, Note, NoteCitation, NoteBody
    d = OpenDocumentText()
    count = {"f": 0, "e": 0}
    for kind, content in BODY:
        if kind == "h":
            d.text.addElement(H(outlinelevel=1, text=content))
            continue
        p = P()
        for r in content:
            if isinstance(r, str):
                p.addText(r)
                continue
            k, text = r
            count[k] += 1
            cls = "footnote" if k == "f" else "endnote"
            n = Note(id="%s%d" % (cls, count[k]), noteclass=cls)
            n.addElement(NoteCitation(text=str(count[k]) if k == "f" else "i" * count[k]))
            body = NoteBody()
            body.addElement(P(text=text))
            n.addElement(body)
            p.addElement(n)
        d.text.addElement(p)
    d.save(path)


make_docx("samples/Notes-Demo.docx")
make_odt("samples/Notes-Demo.odt")
print("ok")
