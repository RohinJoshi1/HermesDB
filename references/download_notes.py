from io import BytesIO

import requests
from pypdf import PdfWriter

base_url = "https://15445.courses.cs.cmu.edu/spring2026/notes/"

pdf_files = [
    "01-relationalmodel.pdf",
    "02-modernsql.pdf",
    "03-storage1.pdf",
    "04-bufferpool.pdf",
    "05-storage2.pdf",
    "06-storage3.pdf",
    "07-hashtables.pdf",
    "08-indexes1.pdf",
    "09-indexes2.pdf",
    "10-indexconcurrency.pdf",
    "11-queryI.pdf",
    "12-joins.pdf",
    "13-queryexecution1.pdf",
    "14-queryexecution2.pdf",
    "15-optimization1.pdf",
    "16-optimization2.pdf",
    "17-concurrencycontrol.pdf",
    "18-twophaselocking.pdf",
    "19-timestampordering.pdf",
    "20-multiversioning1.pdf",
    "21-multiversioning2.pdf",
    "22-logging.pdf",
    "23-recovery.pdf",
    "24-distributed1.pdf",
    "25-distributed2.pdf",
]

writer = PdfWriter()

print("Downloading and merging files...")
for filename in pdf_files:
    file_url = base_url + filename
    print(f"Fetching {filename}...")
    response = requests.get(file_url, headers={"User-Agent": "Mozilla/5.0"}, timeout=60)
    response.raise_for_status()
    writer.append(BytesIO(response.content))

output_filename = "cmu15445_spring2026_all_notes.pdf"
writer.write(output_filename)
writer.close()

print(f"\nDone! All notes merged into '{output_filename}'.")
