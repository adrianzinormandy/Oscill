"""Render the read-back frontbuffer; this is memory evidence, not a board photo."""
from pathlib import Path
import struct, zlib, json

p = Path(__file__).parent
raw = struct.unpack("<76800H", (p / "frame-edge.bin").read_bytes())
lo = struct.unpack("<304H", (p / "columns-min-edge.bin").read_bytes())
hi = struct.unpack("<304H", (p / "columns-max-edge.bin").read_bytes())
mean = [(a+b)/2 for a, b in zip(lo, hi)]
edges = [{"x_logical_pixels": 8+i, "direction": "rising" if a < 2048 else "falling"}
         for i, (a, b) in enumerate(zip(mean, mean[1:]), 1)
         if (a < 2048 <= b) or (a >= 2048 > b)]

def chunk(name, data):
    return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name+data) & 0xffffffff)

def render(scale):
    rows = []
    for y in range(240):
        row = bytearray()
        for x in range(320):
            code = raw[x*240+239-y]
            r, g, b = (code >> 11) & 31, (code >> 5) & 63, code & 31
            row.extend(bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))) * scale)
        rows.extend([b"\0"+row] * scale)
    image = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 320*scale, 240*scale, 8, 2, 0, 0, 0))
    image += chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b"")
    (p / f"frame-edge-{scale}x.png").write_bytes(image)

render(1)
render(3)
summary = {"trigger_marker_x": 68, "column_edges_at_mid_code": edges,
           "samples_of_columns_near_marker": [{"x": x, "min": lo[x-8], "max": hi[x-8]} for x in range(64, 76)],
           "green_y_extent_near_marker": [{"x": x, "y_min": min(ys) if ys else None, "y_max": max(ys) if ys else None}
               for x in range(64, 76) for ys in [[y for y in range(26, 219) if raw[x*240+239-y] == 0x07E0]]]}
(p / "frame-edge-analysis.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
print(json.dumps(summary, indent=2))
