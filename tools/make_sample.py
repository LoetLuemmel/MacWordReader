"""Builds samples/Duo-Demo.docx - a document that exercises the code-table differences."""
from docx import Document
from docx.shared import Pt
d = Document()
d.add_heading("WordReader auf dem PowerBook Duo", 0)
d.add_paragraph("Ein Word-Dokument (.docx) aus dem Jahr 2026, gelesen auf einem Macintosh "
                "von 1992. Word speichert Unicode (UTF-8 im XML), der Duo zeichnet Mac OS Roman.")
d.add_heading("1. Was direkt passt", 1)
d.add_paragraph("Umlaute und ß: Größe, Übermaß, Äpfel, Ölüberschuß. Französisch: café, "
                "garçon, être, Noël. Spanisch: año, ¿qué? ¡olé! Skandinavisch: Århus, Ærø, Øre.")
d.add_paragraph("Typografie: „deutsche Anführung“, “English quotes”, ‘single’, «guillemets», "
                "Gedankenstrich – und Geviertstrich —, Ellipse …, Aufzählungspunkt •, ° ± ≤ ≥ ≠ ∞ π Ω.")
d.add_heading("2. Was ersetzt werden muss", 1)
d.add_paragraph("Euro: 1.299 € (System 7 kennt kein €-Zeichen, erst Mac OS 8.5). "
                "Brüche: ½ Liter, ¾ Stunde. Hochzahlen: 25 m², 3 m³. Mal-Zeichen: 3 × 4.")
d.add_paragraph("Osteuropa: Łódź, Kraków, Dvořák, Škoda, Gdańsk, Ostrava, Şişli.")
d.add_heading("3. Was der Duo nicht zeigen kann", 1)
d.add_paragraph("Griechisch: Αθήνα. Kyrillisch: Москва. Japanisch: 東京. Emoji: 😀 🚀.")
d.add_paragraph("Zerlegte Akzente, wie macOS sie schreibt: Café, München, Niño.")
d.add_heading("Listen", 2)
for t in ["Inflate: 32 KB Fenster, Huffman-Tabellen", "ZIP: zentrales Verzeichnis",
          "XML: Zustandsautomat, Byte für Byte", "Unicode → Mac OS Roman"]:
    d.add_paragraph(t, style="List Bullet")
p = d.add_paragraph("Zweite Ebene: eingerückt", style="List Bullet 2")
d.add_heading("Tabelle: Zeichen und Codes", 2)
rows = [("Zeichen", "Unicode", "Mac OS Roman"), ("ä", "U+00E4", "$8A"), ("ß", "U+00DF", "$A7"),
        ("€", "U+20AC", "„EUR“"), ("„", "U+201E", "$E3"), ("Ł", "U+0141", "„L“"), ("😀", "U+1F600", "?")]
t = d.add_table(rows=len(rows), cols=3)
t.style = "Table Grid"
for i, r in enumerate(rows):
    for j, v in enumerate(r):
        t.cell(i, j).text = v
d.add_heading("Ein längerer Text zum Blättern", 1)
for i in range(1, 16):
    d.add_paragraph(f"Absatz {i}: Der PowerBook Duo 230 hat einen 68030 mit 33 MHz, 4 MB RAM "
                    "auf der Platine und ein 9,1-Zoll-Graustufendisplay mit 640 × 400 Punkten. "
                    "Das Programm entpackt das Dokument im Speicher, ohne es je ganz zu halten: "
                    "Die Daten fließen durch ein 32-KB-Fenster direkt in den XML-Leser.")
d.save("samples/Duo-Demo.docx")
print("ok")
