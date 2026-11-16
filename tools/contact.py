"""Contact sheet of screenshots: python -I tools/contact.py out.png img1 img2 ... (labels = file stems)."""
import sys, os
from PIL import Image, ImageDraw

out, files = sys.argv[1], sys.argv[2:]
cols = 4
tw, th = 320, 240
rows = (len(files) + cols - 1) // cols
sheet = Image.new("RGB", (cols * tw, rows * th), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f).convert("RGB").resize((tw, th))
    x, y = (i % cols) * tw, (i // cols) * th
    sheet.paste(im, (x, y))
    d.text((x + 4, y + 4), os.path.splitext(os.path.basename(f))[0], fill=(255, 255, 0))
sheet.save(out)
