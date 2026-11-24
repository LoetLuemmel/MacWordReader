"""make_sample_odt.py — samples/Duo-Demo.odt, an OpenDocument test file built
the way LibreOffice writes one: named styles with parents in styles.xml,
automatic styles in content.xml, text:h headings, nested lists, a table,
spans, text:s, line breaks, a footnote (shown after the text) and a comment (not shown)."""
import os
from odf.opendocument import OpenDocumentText
from odf.style import Style, TextProperties, ParagraphProperties, DefaultStyle
from odf.text import H, P, Span, List, ListItem, Tab, LineBreak, S, Note, NoteCitation, NoteBody
from odf.table import Table, TableColumn, TableRow, TableCell
from odf.office import Annotation

doc = OpenDocumentText()
st, auto = doc.styles, doc.automaticstyles

d = DefaultStyle(family="paragraph"); d.addElement(TextProperties(fontsize="11pt")); st.addElement(d)
std = Style(name="Standard", family="paragraph"); st.addElement(std)
body = Style(name="Text_20_body", displayname="Text body", family="paragraph", parentstylename="Standard")
body.addElement(ParagraphProperties(margintop="0cm", marginbottom="0.247cm")); st.addElement(body)
head = Style(name="Heading", family="paragraph", parentstylename="Standard")
head.addElement(ParagraphProperties(margintop="0.423cm", marginbottom="0.212cm"))
head.addElement(TextProperties(fontsize="14pt", fontweight="bold")); st.addElement(head)
for n, size in ((1, "130%"), (2, "115%")):
    h = Style(name=f"Heading_20_{n}", displayname=f"Heading {n}", family="paragraph", parentstylename="Heading")
    h.addElement(TextProperties(fontsize=size)); st.addElement(h)
title = Style(name="Title", family="paragraph", parentstylename="Heading")
title.addElement(ParagraphProperties(textalign="center")); title.addElement(TextProperties(fontsize="28pt")); st.addElement(title)

center = Style(name="P1", family="paragraph", parentstylename="Text_20_body")
center.addElement(ParagraphProperties(textalign="center")); auto.addElement(center)
indent = Style(name="P2", family="paragraph", parentstylename="Text_20_body")
indent.addElement(ParagraphProperties(marginleft="1cm", textindent="-0.5cm")); auto.addElement(indent)
right = Style(name="P3", family="paragraph", parentstylename="Text_20_body")
right.addElement(ParagraphProperties(textalign="end")); auto.addElement(right)
B = Style(name="T1", family="text"); B.addElement(TextProperties(fontweight="bold")); auto.addElement(B)
I = Style(name="T2", family="text"); I.addElement(TextProperties(fontstyle="italic")); auto.addElement(I)
U = Style(name="T3", family="text"); U.addElement(TextProperties(textunderlinestyle="solid")); auto.addElement(U)
SM = Style(name="T4", family="text"); SM.addElement(TextProperties(fontsize="8pt")); auto.addElement(SM)

t = doc.text
def para(style, *parts):
    p = P(stylename=style)
    for x in parts:
        p.addText(x) if isinstance(x, str) else p.addElement(x)
    t.addElement(p); return p
def span(style, text):
    s = Span(stylename=style); s.addText(text); return s

tp = P(stylename=title); tp.addText("WordReader liest OpenDocument"); t.addElement(tp)
para(center, "Ein ", span(I, "herstellerneutrales"), " Format: ODF, ISO/IEC 26300 – zentriert.")
para(right, "München, 23. November 2026")
h = H(outlinelevel=1, stylename="Heading_20_1"); h.addText("1. Absätze und Zeichen"); t.addElement(h)
para(body, "Normaler Text mit ", span(B, "fett"), ", ", span(I, "kursiv"), ", ", span(U, "unterstrichen"),
     " und ", span(SM, "klein"), ". Umlaute: Größe, Äpfel, Übermaß. Euro: 1.299 €.")
p = para(body, "Leerzeichen:"); p.addElement(S(c=4)); p.addText("vier, Tab:"); p.addElement(Tab()); p.addText("danach,")
p.addElement(LineBreak()); p.addText("und nach einem Zeilenumbruch im selben Absatz.")
fn = Note(noteclass="footnote"); nc = NoteCitation(); nc.addText("1"); fn.addElement(nc)
nb = NoteBody(); nbp = P(stylename=body); nbp.addText("Der Fußnotentext steht am Ende des Dokuments."); nb.addElement(nbp); fn.addElement(nb)
p = para(body, "Ein Satz mit Fußnote"); p.addElement(fn); p.addText(" und Kommentar")
ann = Annotation(); ap = P(); ap.addText("KOMMENTAR – wird nicht angezeigt"); ann.addElement(ap); p.addElement(ann); p.addText(".")
para(indent, "Hängender Einzug: diese Zeile ist lang genug, um umzubrechen, und die Folgezeilen beginnen 1 cm vom Rand, die erste Zeile 0,5 cm.")
h = H(outlinelevel=2, stylename="Heading_20_2"); h.addText("2. Listen"); t.addElement(h)
lst = List()
for txt in ("Erster Punkt", "Zweiter Punkt mit etwas mehr Text, damit er auf dem Duo in die nächste Zeile umbricht"):
    li = ListItem(); lp = P(stylename=body); lp.addText(txt); li.addElement(lp); lst.addElement(li)
li = ListItem(); lp = P(stylename=body); lp.addText("Dritter Punkt mit Unterliste:"); li.addElement(lp)
sub = List()
for txt in ("eingerückt a", "eingerückt b"):
    sli = ListItem(); sp = P(stylename=body); sp.addText(txt); sli.addElement(sp); sub.addElement(sli)
li.addElement(sub); lst.addElement(li); t.addElement(lst)
h = H(outlinelevel=2, stylename="Heading_20_2"); h.addText("3. Tabelle"); t.addElement(h)
tb = Table(name="T"); tb.addElement(TableColumn(numbercolumnsrepeated=3))
for row in (("Format", "Norm", "Endung"), ("ODF", "ISO/IEC 26300", ".odt"), ("OOXML", "ISO/IEC 29500", ".docx")):
    tr = TableRow()
    for c in row:
        tc = TableCell(); cp = P(); cp.addText(c); tc.addElement(cp); tr.addElement(tc)
    tb.addElement(tr)
t.addElement(tb)
para(body, "Ende.")
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "samples", "Duo-Demo.odt")
doc.save(out)
print(out)
