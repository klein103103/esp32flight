"""Build the radar SD model library from the What is flying? photo catalog."""
import csv
import re
import sys
from pathlib import Path

from PIL import Image, ImageOps

source, output = map(Path, sys.argv[1:])
models = output / "radar" / "models"
models.mkdir(parents=True, exist_ok=True)
credits = []
seen = set()
with (source / "aircraft_types_pictures.csv").open(encoding="utf-8-sig", newline="") as catalog:
    for row in csv.DictReader(catalog):
        code, license_id = row["aircraft_type"], row["license"]
        if not re.fullmatch(r"[A-Z0-9]{2,4}", code) or code in seen:
            continue
        if not re.fullmatch(r"CC-(?:BY(?:-SA)?-[0-9.]+|0-1\.0)|CC0-1\.0|Public Domain|PDM-1\.0", license_id, re.I):
            continue
        author = " ".join(row["author_name"].split())
        if not row["source"].startswith(("https://", "http://")) or (license_id.startswith("CC-BY") and not author):
            continue
        name = Path(row["picture"]).name
        if name != f"{code}.webp":
            continue
        with Image.open(source / "aircraft_types_pictures" / name) as image:
            photo = ImageOps.exif_transpose(image).convert("RGB")
            photo.thumbnail((540, 340), Image.Resampling.LANCZOS)
            photo.save(models / f"{code}.jpg", quality=85, progressive=False, optimize=True)
        label = f"Representative {code}\n{author} / {license_id}"
        (models / f"{code}.txt").write_bytes(label.encode("utf-8")[:127].decode("utf-8", "ignore").encode("utf-8"))
        credits.append({"model": code, "author": author, "license": license_id, "source": row["source"]})
        seen.add(code)

if len(seen) < 100:
    raise SystemExit(f"Catalog unexpectedly small: {len(seen)} usable photos")
with (models / "credits.csv").open("w", encoding="utf-8", newline="") as file:
    writer = csv.DictWriter(file, fieldnames=("model", "author", "license", "source"))
    writer.writeheader()
    writer.writerows(credits)
(models / "README.txt").write_text(
    f"{len(seen)} representative aircraft model photos for esp32flight.\n"
    "Copy the radar folder to the root of the SD card and merge the folders.\n"
    "Photos were resized and converted to JPEG. See credits.csv for attribution and original sources.\n"
    "Source catalog: https://github.com/laegsgaardTroels/whatisflying-db (ODbL-1.0 database).\n"
    "Each photo retains its own license. Model photos are representative, and firmware can replace\n"
    "the displayed thumbnail with an exact aircraft photo when available.\n",
    encoding="utf-8",
)
print(f"Prepared {len(seen)} model JPEGs with attribution")
