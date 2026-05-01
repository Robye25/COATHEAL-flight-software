import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
try:
    import fitz
except ImportError:
    try:
        from pypdf import PdfReader
        r = PdfReader(sys.argv[1])
        for i, p in enumerate(r.pages):
            print(f"===PAGE {i+1}===")
            print(p.extract_text() or "")
        sys.exit(0)
    except ImportError:
        print("no pdf lib"); sys.exit(1)
d = fitz.open(sys.argv[1])
for i, p in enumerate(d):
    print(f"===PAGE {i+1}===")
    print(p.get_text())
