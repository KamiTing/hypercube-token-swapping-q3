import csv
import sys
from copy import copy
from pathlib import Path

from openpyxl import load_workbook
from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
from openpyxl.utils import get_column_letter


ROOT = Path(__file__).resolve().parents[1]
WORKBOOK_PATH = ROOT / "output" / "qk_special_cases_routes.xlsx"
csv.field_size_limit(min(sys.maxsize, 2_147_483_647))

SOURCES = {
    "Q10": [
        ROOT / "output" / "cuda_opt_verify_q10_bw512_case1" / "qk_special_cases.csv",
        ROOT
        / "output"
        / "q10_cuda_bw512_pool32768_win32768_restart1024_plateau1536_perturb010_case2_20260612_222245"
        / "qk_special_cases.csv",
    ],
    "Q11": [
        ROOT
        / "output"
        / "q11_cuda_cub_bw512_pool65536_win65536_restart2048_plateau2048_perturb010_path_20260612_231014"
        / "qk_special_cases.csv",
    ],
}

BLACK = "111827"
SLATE = "334155"
MUTED = "475569"
RED = "EF4444"
BLUE = "2563EB"
TITLE_FILL = "DBEAFE"
HEADER_FILL = "2563EB"
ROUTE_FILL = "F8FAFC"
SUMMARY_FILL = "F1F5F9"
BORDER = Border(
    left=Side(style="thin", color="CBD5E1"),
    right=Side(style="thin", color="CBD5E1"),
    top=Side(style="thin", color="CBD5E1"),
    bottom=Side(style="thin", color="CBD5E1"),
)


def read_rows(path):
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def parse_state(text):
    return [int(x) for x in text.strip().split()]


def parse_path(text):
    if not text or not text.strip():
        return []
    out = []
    for part in text.strip().split():
        u, v = part.split("-", 1)
        out.append((int(u), int(v)))
    return out


def tuple_text(values):
    return "(" + ", ".join(str(v) for v in values) + ")"


def case_number(name):
    marker = "case"
    idx = name.lower().rfind(marker)
    if idx < 0:
        return 999999
    try:
        return int(name[idx + len(marker) :])
    except ValueError:
        return 999999


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


def solved_rows_for_sheet(sheet_name):
    rows = []
    for source in SOURCES[sheet_name]:
        for row in read_rows(source):
            if row.get("beam_status") == "solved" and row.get("beam_path_valid") == "1":
                rows.append(row)
    rows.sort(key=lambda r: case_number(r["case_name"]))
    return rows


def set_cell(cell, value, font=None, fill=None, align=None, border=True):
    cell.value = value
    if font is not None:
        cell.font = font
    if fill is not None:
        cell.fill = fill
    if align is not None:
        cell.alignment = align
    if border:
        cell.border = BORDER


def clear_sheet_layout(ws):
    for merge in list(ws.merged_cells.ranges):
        ws.unmerge_cells(str(merge))
    ws.delete_rows(1, ws.max_row)
    for col in range(1, ws.max_column + 1):
        ws.column_dimensions[get_column_letter(col)].width = 8.43


def write_case(ws, row, start_col, dim):
    state = parse_state(row["state_perm"])
    swaps = parse_path(row["beam_path"])
    current = list(state)
    target = list(range(len(current)))

    font_size = 6
    title_font = Font(name="Calibri", size=font_size, bold=True, color="0F172A")
    header_font = Font(name="Calibri", size=font_size, bold=True, color="FFFFFF")
    body_font = Font(name="Calibri", size=font_size, color=BLACK)
    route_font = Font(name="Calibri", size=font_size, italic=True, color=SLATE)
    summary_font = Font(name="Calibri", size=font_size, bold=True, color="0F172A")
    muted_font = Font(name="Calibri", size=font_size, bold=True, color=MUTED)
    title_fill = PatternFill("solid", fgColor=TITLE_FILL)
    header_fill = PatternFill("solid", fgColor=HEADER_FILL)
    route_fill = PatternFill("solid", fgColor=ROUTE_FILL)
    summary_fill = PatternFill("solid", fgColor=SUMMARY_FILL)
    top_align = Alignment(vertical="top", wrap_text=True)
    center_align = Alignment(horizontal="center", vertical="center", wrap_text=True)

    end_col = start_col + 2
    ws.merge_cells(start_row=1, start_column=start_col, end_row=1, end_column=end_col)
    set_cell(
        ws.cell(1, start_col),
        f"Case {case_number(row['case_name'])}: {row['case_name']}",
        font=title_font,
        fill=title_fill,
        align=center_align,
    )
    for col in range(start_col + 1, end_col + 1):
        ws.cell(1, col).border = BORDER
        ws.cell(1, col).fill = title_fill

    headers = ["Packet Route based on the fixed host labeled", "Permutation", "Outcome"]
    for offset, value in enumerate(headers):
        set_cell(
            ws.cell(2, start_col + offset),
            value,
            font=header_font,
            fill=header_fill,
            align=center_align,
        )

    r = 3
    for i, (u, v) in enumerate(swaps):
        before = list(current)
        current[u], current[v] = current[v], current[u]
        after = list(current)
        after_is_target = after == target
        before_color = BLACK if i == 0 else (RED if i % 2 == 1 else BLUE)
        after_color = BLACK if after_is_target else (RED if i % 2 == 0 else BLUE)

        set_cell(ws.cell(r, start_col), f"Swap ({u}, {v})", font=body_font, align=top_align)
        set_cell(
            ws.cell(r, start_col + 1),
            tuple_text(before),
            font=Font(name="Calibri", size=font_size, color=before_color),
            align=top_align,
        )
        set_cell(
            ws.cell(r, start_col + 2),
            tuple_text(after),
            font=Font(name="Calibri", size=font_size, color=after_color),
            align=top_align,
        )
        r += 1

    if current != target:
        raise RuntimeError(f"{row['case_name']} Beam path does not replay to identity")

    chunks = route_chunks(swaps)
    for i in range(0, len(chunks), 3):
        for offset in range(3):
            set_cell(
                ws.cell(r, start_col + offset),
                chunks[i + offset] if i + offset < len(chunks) else "",
                font=route_font,
                fill=route_fill,
                align=top_align,
            )
        ws.row_dimensions[r].height = 64
        r += 1

    summary = [
        [f"Steps: {len(swaps)}", f"Beam: {row['beam_status']} ({row['beam_steps']})", f"Batcher: {'success' if row['batcher_success'] == '1' else 'fail'} ({row['batcher_swaps']})"],
        [f"Strong LB: {row['strong_lb']}", f"Beam path valid: {row['beam_path_valid']}", f"Batcher path valid: {row['batcher_path_valid']}"],
    ]
    for line_idx, line in enumerate(summary):
        for offset, value in enumerate(line):
            set_cell(
                ws.cell(r, start_col + offset),
                value,
                font=summary_font if line_idx == 0 else muted_font,
                fill=summary_fill,
                align=top_align,
            )
        r += 1

    return r - 1


def format_sheet(ws, dim, max_row, case_count):
    ws.freeze_panes = "A3"
    ws.sheet_view.showGridLines = False
    ws.row_dimensions[1].height = 21
    ws.row_dimensions[2].height = 25.5

    widths = {
        10: (28, 174, 174),
        11: (28, 205, 205),
    }[dim]
    for case_idx in range(case_count):
        start_col = 1 + case_idx * 4
        for offset, width in enumerate(widths):
            ws.column_dimensions[get_column_letter(start_col + offset)].width = width
        if case_idx < case_count - 1:
            ws.column_dimensions[get_column_letter(start_col + 3)].width = 3

    for row in ws.iter_rows(min_row=1, max_row=max_row, max_col=max(3, case_count * 4 - 1)):
        for cell in row:
            if cell.value is None and cell.border == Border():
                continue
            if cell.font:
                copied = copy(cell.font)
                copied.name = "Calibri"
                copied.size = 6
                cell.font = copied


def rebuild_sheet(wb, title, rows, dim, index):
    if title in wb.sheetnames:
        del wb[title]
    ws = wb.create_sheet(title, index)
    clear_sheet_layout(ws)

    max_row = 1
    for case_idx, row in enumerate(rows):
        max_row = max(max_row, write_case(ws, row, 1 + case_idx * 4, dim))

    format_sheet(ws, dim, max_row, len(rows))
    return ws


def main():
    wb = load_workbook(WORKBOOK_PATH)
    q10_rows = solved_rows_for_sheet("Q10")
    q11_rows = solved_rows_for_sheet("Q11")
    if len(q10_rows) != 2:
        raise RuntimeError(f"Expected 2 Q10 rows, found {len(q10_rows)}")
    if len(q11_rows) != 2:
        raise RuntimeError(f"Expected 2 Q11 rows, found {len(q11_rows)}")

    q10_index = wb.sheetnames.index("Q10") if "Q10" in wb.sheetnames else len(wb.sheetnames)
    rebuild_sheet(wb, "Q10", q10_rows, 10, q10_index)
    q11_index = wb.sheetnames.index("Q10") + 1
    rebuild_sheet(wb, "Q11", q11_rows, 11, q11_index)

    wb.save(WORKBOOK_PATH)
    print(
        {
            "workbook": str(WORKBOOK_PATH),
            "Q10": [(r["case_name"], r["beam_steps"]) for r in q10_rows],
            "Q11": [(r["case_name"], r["beam_steps"]) for r in q11_rows],
        }
    )


if __name__ == "__main__":
    main()
