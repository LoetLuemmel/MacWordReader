"""make_sample_pictures.py — samples/Pictures-Demo.docx and .odt.

All pictures are drawn here (no outside images):
  diagram.png      480x200 RGBA, transparent background: the reading pipeline
  landscape.jpg    1600x1000 baseline JPEG 4:2:0 - a photo-sized picture
  rings.jpg        640x400 progressive JPEG
  chart.png        120x90 8-bit grey, shown larger than it is
  dots.gif         a GIF: WordReader shows a labelled box instead
The .docx stores its media deflated (as python-docx and Word do); the .odt
stores Pictures/ uncompressed (as LibreOffice does), so both paths are used.
uv run --with python-docx --with odfpy --with pillow --with numpy python tools/make_sample_pictures.py
"""
import io, os, zipfile
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "samples")
rng = np.random.default_rng(1992)


def font(size):
    for f in ("/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf"):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def diagram():
    im = Image.new("RGBA", (480, 200), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    steps = ["ZIP", "inflate", "XML", "Mac OS\nRoman", "Quick-\nDraw"]
    fills = [(70, 110, 200), (60, 160, 90), (220, 150, 40), (180, 60, 60), (110, 80, 170)]
    for i, (s, c) in enumerate(zip(steps, fills)):
        x = 10 + i * 95
        d.rounded_rectangle([x, 60, x + 72, 140], 10, fill=c + (255,), outline=(0, 0, 0, 255), width=2)
        d.multiline_text((x + 36, 100), s, font=font(15), fill=(255, 255, 255, 255), anchor="mm", align="center")
        if i < 4:
            d.line([x + 74, 100, x + 93, 100], fill=(0, 0, 0, 255), width=3)
            d.polygon([(x + 93, 100), (x + 85, 94), (x + 85, 106)], fill=(0, 0, 0, 255))
    d.text((240, 30), ".docx / .odt  ->  pixels on a 1992 PowerBook", font=font(16), fill=(0, 0, 0, 255), anchor="mm")
    d.text((240, 170), "transparent background", font=font(12), fill=(0, 0, 0, 160), anchor="mm")
    return im


def landscape(w=1600, h=1000):
    y, x = np.mgrid[0:h, 0:w].astype(float)
    sky = np.dstack([90 + 100 * y / h, 140 + 80 * y / h, 230 - 30 * y / h])
    img = sky.copy()
    sun = (x - w * 0.72) ** 2 + (y - h * 0.25) ** 2 < (h * 0.08) ** 2
    img[sun] = [255, 235, 150]
    for k, (base, amp, col) in enumerate([(0.55, 0.12, (90, 100, 120)), (0.65, 0.08, (60, 110, 60)),
                                          (0.75, 0.05, (40, 80, 40))]):
        ridge = h * (base + amp * np.sin(x[0] / w * (3 + 2 * k) * np.pi + k)
                     + 0.02 * np.sin(x[0] / w * 37 * np.pi))
        img[y > ridge[None, :]] = col
    lake = y > h * 0.85
    img[lake] = img[(2 * int(h * 0.85) - y[lake]).astype(int), x[lake].astype(int)] * 0.7 + [10, 20, 40]
    img += rng.normal(0, 6, img.shape)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


def rings(w=640, h=400):
    y, x = np.mgrid[0:h, 0:w].astype(float)
    r = np.hypot(x - w / 2, y - h / 2)
    img = np.dstack([128 + 127 * np.sin(r / 9), 128 + 127 * np.sin(r / 13 + 1), 128 + 127 * np.cos(x / 40)])
    im = Image.fromarray(img.astype(np.uint8), "RGB")
    ImageDraw.Draw(im).text((w / 2, h / 2), "progressive JPEG", font=font(36), fill=(0, 0, 0), anchor="mm")
    return im


def chart():
    im = Image.new("L", (120, 90), 255)
    d = ImageDraw.Draw(im)
    for i, v in enumerate([30, 55, 42, 70, 64, 80]):
        d.rectangle([8 + i * 18, 85 - v, 20 + i * 18, 85], fill=40 + i * 30)
    d.line([4, 85, 116, 85], fill=0)
    return im


def dots():
    im = Image.new("P", (80, 60), 0)
    d = ImageDraw.Draw(im)
    for i in range(12):
        d.ellipse([i * 6, i * 4, i * 6 + 20, i * 4 + 20], fill=i + 1)
    return im


def save(im, fmt, **kw):
    b = io.BytesIO()
    im.save(b, fmt, **kw)
    return b.getvalue()


PICS = {
    "diagram.png": save(diagram(), "PNG"),
    "landscape.jpg": save(landscape(), "JPEG", quality=85, subsampling=2),
    "rings.jpg": save(rings(), "JPEG", quality=80, progressive=True),
    "chart.png": save(chart(), "PNG"),
    "dots.gif": save(dots(), "GIF"),
}

TEXT = [
    ("h0", "Bilder auf dem PowerBook Duo"),
    ("p", "WordReader zeichnet PNG- und JPEG-Bilder selbst: verkleinert beim Lesen, in 16 Graustufen."),
    ("h1", "1. Ein Diagramm (PNG mit Transparenz)"),
    ("pic", "diagram.png", 4.5),
    ("c", "Abbildung 1: Der Weg vom Dokument zum Bildschirm."),
    ("h1", "2. Ein Foto (JPEG, 1600 x 1000 Pixel)"),
    ("p", "Das Bild ist breiter als das Fenster und wird beim Dekodieren verkleinert."),
    ("pic", "landscape.jpg", 6.0),
    ("c", "Abbildung 2: Berge am See, frisch gerechnet."),
    ("h1", "3. Progressives JPEG und ein kleines Diagramm"),
    ("pic", "rings.jpg", 3.0),
    ("p", "Ein kleines Graustufen-PNG, groesser gezeigt als es ist:"),
    ("pic", "chart.png", 2.0),
    ("h1", "4. Was nicht gezeichnet wird"),
    ("p", "Ein GIF wird als beschrifteter Rahmen gezeigt:"),
    ("pic", "dots.gif", 1.5),
    ("p", "Ende."),
]


def make_docx(path):
    from docx import Document
    from docx.shared import Inches
    from docx.enum.text import WD_ALIGN_PARAGRAPH
    d = Document()
    for item in TEXT:
        kind = item[0]
        if kind.startswith("h"):
            d.add_heading(item[1], int(kind[1]))
        elif kind == "pic":
            d.add_picture(io.BytesIO(PICS[item[1]]), width=Inches(item[2]))
            d.paragraphs[-1].alignment = WD_ALIGN_PARAGRAPH.CENTER
        else:
            p = d.add_paragraph(item[1])
            if kind == "c":
                p.alignment = WD_ALIGN_PARAGRAPH.CENTER
                p.runs[0].italic = True
    d.save(path)


def make_odt(path):
    from odf.opendocument import OpenDocumentText
    from odf.style import Style, TextProperties, ParagraphProperties, GraphicProperties
    from odf.text import H, P
    from odf.draw import Frame, Image as DImage
    doc = OpenDocumentText()
    center = Style(name="Center", family="paragraph")
    center.addElement(ParagraphProperties(textalign="center"))
    doc.automaticstyles.addElement(center)
    cap = Style(name="Caption", family="paragraph")
    cap.addElement(ParagraphProperties(textalign="center"))
    cap.addElement(TextProperties(fontstyle="italic"))
    doc.automaticstyles.addElement(cap)
    fr = Style(name="fr1", family="graphic")
    fr.addElement(GraphicProperties(wrap="none"))
    doc.automaticstyles.addElement(fr)
    for item in TEXT:
        kind = item[0]
        if kind.startswith("h"):
            doc.text.addElement(H(outlinelevel=max(1, int(kind[1])), text=item[1]))
        elif kind == "pic":
            name, inches = item[1], item[2]
            w, h = Image.open(io.BytesIO(PICS[name])).size
            href = doc.addPicture("Pictures/" + name, mediatype=None, content=PICS[name])
            f = Frame(stylename=fr, width=f"{inches * 2.54:.3f}cm", height=f"{inches * 2.54 * h / w:.3f}cm",
                      anchortype="as-char")
            f.addElement(DImage(href=href))
            p = P(stylename=center)
            p.addElement(f)
            doc.text.addElement(p)
        else:
            doc.text.addElement(P(stylename=cap if kind == "c" else None, text=item[1]))
    tmp = path + ".tmp"
    doc.save(tmp)
    with zipfile.ZipFile(tmp) as zi, zipfile.ZipFile(path, "w") as zo:
        for info in zi.infolist():
            stored = info.filename == "mimetype" or info.filename.startswith("Pictures/")
            zo.writestr(info.filename, zi.read(info),
                        compress_type=zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED)
    os.remove(tmp)


make_docx(os.path.join(OUT, "Pictures-Demo.docx"))
make_odt(os.path.join(OUT, "Pictures-Demo.odt"))
for n, b in PICS.items():
    print(f"{n:14s} {len(b):7d} bytes")
print("wrote samples/Pictures-Demo.docx and .odt")
