from io import BytesIO

import requests
from pypdf import PdfWriter

base_url = "https://www.cs.cmu.edu/~15721-f25/notes/"

pdf_files = [
    "01_QO1.pdf",
    "02_QO2.pdf",
    "03_QO3.pdf",
    "04_Q04.pdf",
    "05_Q05.pdf",
    "06_QP2.pdf",
    "07_QP3.pdf",
    "08_TM1.pdf",
    "09_TM2.pdf",
    "10_TM3.pdf",
    "12_TM5.pdf",
    "14_MDP1.pdf",
    "15_MDP1.pdf",
    "16_MDP3.pdf",
]

writer = PdfWriter()

print("Downloading and merging files...")
for filename in pdf_files:
    file_url = base_url + filename
    print(f"Fetching {filename}...")
    response = requests.get(file_url, headers={"User-Agent": "Mozilla/5.0"}, timeout=120)
    response.raise_for_status()
    writer.append(BytesIO(response.content))

output_filename = "cmu15721_fall2025_all_notes.pdf"
writer.write(output_filename)
writer.close()

print(f"\nDone! All notes merged into '{output_filename}'.")
