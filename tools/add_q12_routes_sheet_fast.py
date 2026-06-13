import csv
import html
import os
import re
import shutil
import sys
import tempfile
import time
import zipfile
from pathlib import Path
from xml.etree import ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
WORKBOOK_PATH = ROOT / "output" / "qk_special_cases_routes.xlsx"
Q12_CSV = (
    ROOT
    / "output"
    / "q12_cuda_gpu_retained_bw1024_pool262144_win65536_restart4096_plateau1536_perturb015_cub_path_20260613_132118"
    / "qk_special_cases.csv"
)

MAIN_NS = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
OFFICE_REL_NS = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
PKG_REL_NS = "http://schemas.openxmlformats.org/package/2006/relationships"
CT_NS = "http://schemas.openxmlformats.org/package/2006/content-types"

ET.register_namespace("", MAIN_NS)
ET.register_namespace("r", OFFICE_REL_NS)
ET.register_namespace("", PKG_REL_NS)

csv.field_size_limit(min(sys.maxsize, 2_147_483_647))


def parse_path(text):
    if not text or not text.strip():
        return []
    steps = []
    for part in text.strip().split():
        u, v = part.split("-", 1)
        steps.append((int(u), int(v)))
    return steps


def parse_state(text):
    return [int(x) for x in text.strip().split()]


def read_q12_row():
    with Q12_CSV.open("r", encoding="utf-8", newline="") as f:
        rows = [
            row
            for row in csv.DictReader(f)
            if row.get("case_name") == "q12_case1"
            and row.get("beam_status") == "solved"
            and row.get("beam_path_valid") == "1"
        ]
    if len(rows) != 1:
        raise RuntimeError(f"Expected exactly one solved q12_case1 row, found {len(rows)}")
    return rows[0]


def cell_ref(col, row):
    name = ""
    while col:
        col, rem = divmod(col - 1, 26)
        name = chr(ord("A") + rem) + name
    return f"{name}{row}"


def inline_cell(col, row, style, value):
    ref = cell_ref(col, row)
    escaped = html.escape(value, quote=False)
    return f'<c r="{ref}" s="{style}" t="inlineStr"><is><t>{escaped}</t></is></c>'


def empty_cell(col, row, style):
    ref = cell_ref(col, row)
    return f'<c r="{ref}" s="{style}" t="n"></c>'


def row_xml(row_idx, cells, height=None):
    ht = f' ht="{height}" customHeight="1"' if height is not None else ""
    return f'<row r="{row_idx}"{ht}>{"".join(cells)}</row>\n'


def route_chunks(swaps, max_chars=28000):
    chunks = []
    current = "Route "
    for u, v in swaps:
        piece = f"({u}, {v})"
        next_text = f"{current}{piece}" if current == "Route " else f"{current}, {piece}"
        if len(next_text) > max_chars and current != "Route ":
            chunks.append(current)
            current = piece
        else:
            current = next_text
    if current and current != "Route ":
        chunks.append(current)
    return chunks or ["Route"]


def tuple_text(values):
    return "(" + ", ".join(str(v) for v in values) + ")"


def write_sheet_xml(stream, row):
    state = parse_state(row["state_perm"])
    swaps = parse_path(row["beam_path"])
    current = list(state)
    target = list(range(len(current)))
    route_rows = len(route_chunks(swaps))
    max_row = 2 + len(swaps) + route_rows + 2

    def write(text):
        stream.write(text.encode("utf-8"))

    write('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n')
    write(f'<worksheet xmlns="{MAIN_NS}">')
    write("<sheetPr><outlinePr summaryBelow=\"1\" summaryRight=\"1\"/><pageSetUpPr/></sheetPr>")
    write(f'<dimension ref="A1:C{max_row}"/>')
    write(
        '<sheetViews><sheetView showGridLines="0" workbookViewId="0">'
        '<pane ySplit="2" topLeftCell="A3" activePane="bottomLeft" state="frozen"/>'
        '<selection pane="bottomLeft" activeCell="A1" sqref="A1"/>'
        "</sheetView></sheetViews>"
    )
    write('<sheetFormatPr baseColWidth="8" defaultRowHeight="15"/>')
    write(
        '<cols>'
        '<col width="28" customWidth="1" min="1" max="1"/>'
        '<col width="245" customWidth="1" min="2" max="2"/>'
        '<col width="245" customWidth="1" min="3" max="3"/>'
        "</cols>"
    )
    write("<sheetData>\n")

    write(
        row_xml(
            1,
            [
                inline_cell(1, 1, 49, "Case 1: q12_case1"),
                empty_cell(2, 1, 50),
                empty_cell(3, 1, 50),
            ],
            "21",
        )
    )
    write(
        row_xml(
            2,
            [
                inline_cell(1, 2, 51, "Packet Route based on the fixed host labeled"),
                inline_cell(2, 2, 51, "Permutation"),
                inline_cell(3, 2, 51, "Outcome"),
            ],
            "25.5",
        )
    )

    start_time = time.time()
    for i, (u, v) in enumerate(swaps):
        before = tuple_text(current)
        current[u], current[v] = current[v], current[u]
        after = tuple_text(current)
        after_is_target = current == target
        before_style = 52 if i == 0 else (53 if i % 2 == 1 else 54)
        after_style = 52 if after_is_target else (53 if i % 2 == 0 else 54)
        r = 3 + i
        write(
            row_xml(
                r,
                [
                    inline_cell(1, r, 52, f"Swap ({u}, {v})"),
                    inline_cell(2, r, before_style, before),
                    inline_cell(3, r, after_style, after),
                ],
            )
        )
        if (i + 1) % 1000 == 0:
            elapsed = time.time() - start_time
            print(f"wrote {i + 1}/{len(swaps)} swap rows in {elapsed:.1f}s", flush=True)

    if current != target:
        raise RuntimeError("q12_case1 Beam path does not replay to identity")

    r = 3 + len(swaps)
    for chunk in route_chunks(swaps):
        write(row_xml(r, [inline_cell(2, r, 55, chunk)], "64"))
        r += 1

    write(
        row_xml(
            r,
            [
                inline_cell(1, r, 56, f"Steps: {len(swaps)}"),
                inline_cell(2, r, 56, f"Beam: {row['beam_status']} ({row['beam_steps']})"),
                inline_cell(
                    3,
                    r,
                    56,
                    f"Batcher: {'success' if row['batcher_success'] == '1' else 'fail'} ({row['batcher_swaps']})",
                ),
            ],
        )
    )
    r += 1
    write(
        row_xml(
            r,
            [
                inline_cell(1, r, 57, f"Strong LB: {row['strong_lb']}"),
                inline_cell(2, r, 57, f"Beam path valid: {row['beam_path_valid']}"),
                inline_cell(3, r, 57, f"Batcher path valid: {row['batcher_path_valid']}"),
            ],
        )
    )
    write("</sheetData>")
    write('<mergeCells count="1"><mergeCell ref="A1:C1"/></mergeCells>')
    write("</worksheet>")
    return max_row


def qname(ns, name):
    return f"{{{ns}}}{name}"


def remove_q12_metadata(workbook_root, rels_root, content_root):
    sheets = workbook_root.find(qname(MAIN_NS, "sheets"))
    q12_rids = []
    for sheet in list(sheets):
        if sheet.attrib.get("name") == "Q12":
            rid = sheet.attrib.get(qname(OFFICE_REL_NS, "id"))
            if rid:
                q12_rids.append(rid)
            sheets.remove(sheet)

    for rel in list(rels_root):
        if rel.attrib.get("Id") in q12_rids or rel.attrib.get("Target") == "worksheets/sheet9.xml":
            rels_root.remove(rel)

    for override in list(content_root):
        if override.attrib.get("PartName") == "/xl/worksheets/sheet9.xml":
            content_root.remove(override)


def add_q12_metadata(workbook_root, rels_root, content_root):
    sheets = workbook_root.find(qname(MAIN_NS, "sheets"))
    max_sheet_id = max(int(sheet.attrib.get("sheetId", "0")) for sheet in sheets)
    rel_ids = []
    for rel in rels_root:
        match = re.fullmatch(r"rId(\d+)", rel.attrib.get("Id", ""))
        if match:
            rel_ids.append(int(match.group(1)))
    rid = f"rId{max(rel_ids, default=0) + 1}"

    sheet = ET.Element(qname(MAIN_NS, "sheet"))
    sheet.set("name", "Q12")
    sheet.set("sheetId", str(max_sheet_id + 1))
    sheet.set(qname(OFFICE_REL_NS, "id"), rid)
    sheets.append(sheet)

    rel = ET.Element("Relationship")
    rel.set("Id", rid)
    rel.set("Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet")
    rel.set("Target", "worksheets/sheet9.xml")
    rels_root.append(rel)

    override = ET.Element(qname(CT_NS, "Override"))
    override.set("PartName", "/xl/worksheets/sheet9.xml")
    override.set(
        "ContentType",
        "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml",
    )
    content_root.append(override)


def xml_bytes(root):
    return ET.tostring(root, encoding="utf-8", xml_declaration=True)


def main():
    row = read_q12_row()
    if not WORKBOOK_PATH.exists():
        raise RuntimeError(f"Missing workbook: {WORKBOOK_PATH}")

    tmp_fd, tmp_name = tempfile.mkstemp(
        prefix="qk_routes_q12_", suffix=".xlsx", dir=str(WORKBOOK_PATH.parent)
    )
    os.close(tmp_fd)
    tmp_path = Path(tmp_name)

    try:
        with zipfile.ZipFile(WORKBOOK_PATH, "r") as zin:
            workbook_root = ET.fromstring(zin.read("xl/workbook.xml"))
            rels_root = ET.fromstring(zin.read("xl/_rels/workbook.xml.rels"))
            content_root = ET.fromstring(zin.read("[Content_Types].xml"))
            remove_q12_metadata(workbook_root, rels_root, content_root)
            add_q12_metadata(workbook_root, rels_root, content_root)

            skip = {
                "xl/workbook.xml",
                "xl/_rels/workbook.xml.rels",
                "[Content_Types].xml",
                "xl/worksheets/sheet9.xml",
            }
            with zipfile.ZipFile(tmp_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zout:
                for info in zin.infolist():
                    if info.filename in skip:
                        continue
                    zout.writestr(info, zin.read(info.filename))

                zout.writestr("[Content_Types].xml", xml_bytes(content_root))
                zout.writestr("xl/workbook.xml", xml_bytes(workbook_root))
                zout.writestr("xl/_rels/workbook.xml.rels", xml_bytes(rels_root))
                with zout.open("xl/worksheets/sheet9.xml", "w") as sheet_stream:
                    max_row = write_sheet_xml(sheet_stream, row)

        shutil.move(str(tmp_path), WORKBOOK_PATH)
        print(
            {
                "workbook": str(WORKBOOK_PATH),
                "sheet": "Q12",
                "case": row["case_name"],
                "steps": row["beam_steps"],
                "max_row": max_row,
                "size_bytes": WORKBOOK_PATH.stat().st_size,
            }
        )
    finally:
        if tmp_path.exists():
            tmp_path.unlink()


if __name__ == "__main__":
    main()
