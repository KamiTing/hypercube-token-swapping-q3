import fs from "node:fs/promises";
import path from "node:path";
import { SpreadsheetFile, Workbook } from "@oai/artifact-tool";

const cwd = process.cwd();

const sourceFiles = [
  path.join(cwd, "output", "qk_custom_cases_20260605_133247", "qk_special_cases.csv"),
  path.join(cwd, "output", "q8_case1_trim_20260605_154219", "qk_special_cases.csv"),
  path.join(cwd, "output", "q8_case2_trim_20260605_154927", "qk_special_cases.csv"),
  path.join(cwd, "output", "q9_case1_disk_bw256_20260607_113040", "qk_special_cases.csv"),
  path.join(cwd, "output", "q9_case2_disk_bw256_path_20260609_170556", "qk_special_cases.csv"),
];

const outputDir = path.join(cwd, "output");
const outputXlsx = path.join(outputDir, "qk_special_cases_routes.xlsx");
const renderDir = path.join(outputDir, "qk_special_cases_routes_preview");

function parseCsv(text) {
  const rows = [];
  let row = [];
  let field = "";
  let inQuotes = false;

  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (inQuotes) {
      if (ch === '"') {
        if (text[i + 1] === '"') {
          field += '"';
          i++;
        } else {
          inQuotes = false;
        }
      } else {
        field += ch;
      }
      continue;
    }

    if (ch === '"') {
      inQuotes = true;
    } else if (ch === ",") {
      row.push(field);
      field = "";
    } else if (ch === "\n") {
      row.push(field);
      rows.push(row);
      row = [];
      field = "";
    } else if (ch !== "\r") {
      field += ch;
    }
  }

  if (field.length || row.length) {
    row.push(field);
    rows.push(row);
  }

  return rows;
}

async function readCsvObjects(file) {
  const text = await fs.readFile(file, "utf8");
  const rows = parseCsv(text).filter((r) => r.length && r.some((v) => v !== ""));
  const headers = rows[0];
  return rows.slice(1).map((r) => Object.fromEntries(headers.map((h, i) => [h, r[i] ?? ""])));
}

function parseState(text) {
  return text.trim().split(/\s+/).filter(Boolean).map(Number);
}

function parsePath(text) {
  if (!text || !text.trim()) return [];
  return text.trim().split(/\s+/).map((part) => {
    const [u, v] = part.split("-").map(Number);
    return [u, v];
  });
}

function tuple(values) {
  return `(${values.join(", ")})`;
}

function routeChunks(pathSteps, maxChars = 28000) {
  const pieces = pathSteps.map(([u, v]) => `(${u}, ${v})`);
  const chunks = [];
  let current = "Route ";

  for (const piece of pieces) {
    const next = current === "Route " ? `${current}${piece}` : `${current}, ${piece}`;
    if (next.length > maxChars && current !== "Route ") {
      chunks.push(current);
      current = piece;
    } else {
      current = next;
    }
  }

  if (current && current !== "Route ") chunks.push(current);
  return chunks.length ? chunks : ["Route"];
}

function caseNumber(name) {
  const m = name.match(/case(\d+)$/i);
  return m ? Number(m[1]) : Number.MAX_SAFE_INTEGER;
}

function colName(index) {
  let n = index;
  let out = "";
  while (n > 0) {
    n--;
    out = String.fromCharCode(65 + (n % 26)) + out;
    n = Math.floor(n / 26);
  }
  return out;
}

function cell(col, row) {
  return `${colName(col)}${row}`;
}

function rangeAddress(startCol, startRow, endCol, endRow) {
  return `${cell(startCol, startRow)}:${cell(endCol, endRow)}`;
}

function buildCaseMatrix(row) {
  const state = parseState(row.state_perm);
  const swaps = parsePath(row.beam_path);
  const matrix = [];
  const textColors = [];
  let current = [...state];
  const target = current.map((_, i) => i);

  matrix.push([`Case ${caseNumber(row.case_name)}: ${row.case_name}`, "", ""]);
  textColors.push(["#0F172A", "#0F172A", "#0F172A"]);
  matrix.push([
    "Packet Route based on the fixed host labeled",
    "Permutation",
    "Outcome",
  ]);
  textColors.push(["#FFFFFF", "#FFFFFF", "#FFFFFF"]);

  for (let i = 0; i < swaps.length; i++) {
    const [u, v] = swaps[i];
    const before = [...current];
    [current[u], current[v]] = [current[v], current[u]];
    const after = [...current];
    const afterIsTarget = after.every((value, idx) => value === target[idx]);
    const beforeColor = i === 0 ? "#111827" : (i % 2 === 1 ? "#EF4444" : "#2563EB");
    const afterColor = afterIsTarget ? "#111827" : (i % 2 === 0 ? "#EF4444" : "#2563EB");
    matrix.push([`Swap (${u}, ${v})`, tuple(before), tuple(after)]);
    textColors.push(["#111827", beforeColor, afterColor]);
  }

  if (!current.every((value, idx) => value === target[idx])) {
    throw new Error(`${row.case_name} Beam path does not replay to the identity permutation`);
  }

  const chunks = routeChunks(swaps);
  const routeRowCount = Math.ceil(chunks.length / 3);
  for (let i = 0; i < routeRowCount; i++) {
    matrix.push([
      chunks[i * 3] ?? "",
      chunks[i * 3 + 1] ?? "",
      chunks[i * 3 + 2] ?? "",
    ]);
    textColors.push(["#334155", "#334155", "#334155"]);
  }

  matrix.push([
    `Steps: ${swaps.length}`,
    `Beam: ${row.beam_status} (${row.beam_steps})`,
    `Batcher: ${row.batcher_success === "1" ? "success" : "fail"} (${row.batcher_swaps})`,
  ]);
  textColors.push(["#0F172A", "#0F172A", "#0F172A"]);
  matrix.push([
    `Strong LB: ${row.strong_lb}`,
    `Beam path valid: ${row.beam_path_valid}`,
    `Batcher path valid: ${row.batcher_path_valid}`,
  ]);
  textColors.push(["#475569", "#475569", "#475569"]);

  return { matrix, textColors, routeRowCount };
}

function applyCaseFormatting(sheet, startCol, startRow, rows, colors, dim, routeRowCount) {
  const endCol = startCol + 2;
  const endRow = startRow + rows.length - 1;
  const titleRange = sheet.getRange(rangeAddress(startCol, startRow, endCol, startRow));
  titleRange.merge();
  titleRange.format = {
    fill: "#DBEAFE",
    font: { bold: true, color: "#0F172A" },
    horizontalAlignment: "center",
    verticalAlignment: "center",
  };

  const headerRange = sheet.getRange(rangeAddress(startCol, startRow + 1, endCol, startRow + 1));
  headerRange.format = {
    fill: "#2563EB",
    font: { bold: true, color: "#FFFFFF" },
    horizontalAlignment: "center",
    verticalAlignment: "center",
    wrapText: true,
  };

  const bodyRange = sheet.getRange(rangeAddress(startCol, startRow, endCol, endRow));
  bodyRange.format.borders = { preset: "all", style: "thin", color: "#CBD5E1" };
  bodyRange.format.wrapText = true;
  bodyRange.format.verticalAlignment = "top";

  const widthByDim = {
    4: [240, 360, 360],
    5: [230, 430, 430],
    6: [220, 560, 560],
    7: [210, 720, 720],
    8: [200, 920, 920],
    9: [200, 1040, 1040],
    10: [200, 1220, 1220],
    11: [200, 1440, 1440],
  };
  const [routeWidth, permWidth, outcomeWidth] = widthByDim[dim] ?? widthByDim[4];
  sheet.getRange(rangeAddress(startCol, startRow, startCol, endRow)).format.columnWidthPx = routeWidth;
  sheet.getRange(rangeAddress(startCol + 1, startRow, startCol + 1, endRow)).format.columnWidthPx = permWidth;
  sheet.getRange(rangeAddress(startCol + 2, startRow, startCol + 2, endRow)).format.columnWidthPx = outcomeWidth;

  const routeStartRow = endRow - 1 - routeRowCount;
  const routeEndRow = routeStartRow + routeRowCount - 1;
  if (routeRowCount === 1) {
    sheet.getRange(rangeAddress(startCol, routeStartRow, endCol, routeStartRow)).merge();
  }
  sheet.getRange(rangeAddress(startCol, routeStartRow, endCol, routeEndRow)).format = {
    fill: "#F8FAFC",
    font: { italic: true, color: "#334155" },
    wrapText: true,
    verticalAlignment: "top",
  };
  sheet.getRange(rangeAddress(startCol, routeStartRow, startCol, routeEndRow)).format.rowHeightPx = routeRowCount === 1 ? 64 : 86;

  const stepsRange = sheet.getRange(rangeAddress(startCol, endRow - 1, endCol, endRow));
  stepsRange.format = {
    fill: "#F1F5F9",
    font: { bold: true, color: "#0F172A" },
    wrapText: true,
  };

  for (let r = 0; r < colors.length; r++) {
    for (let c = 0; c < 3; c++) {
      if (r === 1) continue;
      sheet.getRange(cell(startCol + c, startRow + r)).format.font = {
        color: colors[r][c],
        bold: r === 0 || r >= colors.length - 2,
        italic: r === colors.length - 3,
      };
    }
  }
}

async function main() {
  const allRows = [];
  for (const file of sourceFiles) {
    const rows = await readCsvObjects(file);
    for (const row of rows) {
      if (row.beam_status === "solved" && row.beam_path_valid === "1") {
        allRows.push(row);
      }
    }
  }

  const byDim = new Map();
  for (const row of allRows) {
    const dim = Number(row.dim);
    if (dim < 4 || dim > 9) continue;
    if (!byDim.has(dim)) byDim.set(dim, []);
    byDim.get(dim).push(row);
  }
  for (const rows of byDim.values()) {
    rows.sort((a, b) => caseNumber(a.case_name) - caseNumber(b.case_name));
  }

  const workbook = Workbook.create();
  for (const dim of [4, 5, 6, 7, 8, 9]) {
    const rows = byDim.get(dim) ?? [];
    const sheet = workbook.worksheets.add(`Q${dim}`);
    sheet.freezePanes.freezeRows(2);
    sheet.getRange("A1:A1").format.rowHeightPx = 28;
    sheet.getRange("A2:A2").format.rowHeightPx = 34;

    let startCol = 1;
    let maxEndRow = 1;
    for (const row of rows) {
      const { matrix, textColors, routeRowCount } = buildCaseMatrix(row);
      const startRow = 1;
      const endRow = startRow + matrix.length - 1;
      sheet.getRange(rangeAddress(startCol, startRow, startCol + 2, endRow)).values = matrix;
      applyCaseFormatting(sheet, startCol, startRow, matrix, textColors, dim, routeRowCount);
      maxEndRow = Math.max(maxEndRow, endRow);
      startCol += 4;
    }

    if (!rows.length) {
      sheet.getRange("A1:C1").values = [[`Q${dim}`, "No solved Beam route found", ""]];
    } else {
      for (let spacerCol = 4; spacerCol < startCol; spacerCol += 4) {
        sheet.getRange(rangeAddress(spacerCol, 1, spacerCol, maxEndRow)).format.columnWidthPx = 22;
      }
      sheet.getRange(rangeAddress(1, 1, Math.max(1, startCol - 2), maxEndRow)).format.font = {
        name: "Calibri",
        size: dim >= 10 ? 6 : dim >= 9 ? 7 : dim >= 8 ? 8 : dim >= 7 ? 9 : dim >= 6 ? 9 : dim >= 5 ? 10 : 11,
      };
    }
  }

  await fs.mkdir(outputDir, { recursive: true });
  await fs.mkdir(renderDir, { recursive: true });

  for (const dim of [4, 5, 6, 7, 8, 9]) {
    const image = await workbook.render({ sheetName: `Q${dim}`, range: "A1:L24", scale: 1.25 });
    await fs.writeFile(path.join(renderDir, `Q${dim}.png`), Buffer.from(await image.arrayBuffer()));
  }

  const errors = await workbook.inspect({
    kind: "match",
    searchTerm: "#REF!|#DIV/0!|#VALUE!|#NAME\\?|#N/A",
    options: { useRegex: true, maxResults: 50 },
    summary: "formula error scan",
  });
  if (errors.ndjson.split(/\r?\n/).some((line) => line.includes('"kind":"match"'))) {
    console.error(errors.ndjson);
    throw new Error("Workbook contains spreadsheet error tokens");
  }

  const q9Summary = await workbook.inspect({
    kind: "table",
    range: "Q9!A1562:C1563",
    include: "values,formulas",
    tableMaxRows: 2,
    tableMaxCols: 3,
  });
  if (
    !q9Summary.ndjson.includes("Steps: 1558") ||
    !q9Summary.ndjson.includes("Beam: solved (1558)") ||
    !q9Summary.ndjson.includes("Beam path valid: 1")
  ) {
    throw new Error(`Unexpected Q9 summary cells:\n${q9Summary.ndjson}`);
  }

  const blob = await SpreadsheetFile.exportXlsx(workbook);
  await blob.save(outputXlsx);

  console.log(JSON.stringify({
    outputXlsx,
    sheets: Object.fromEntries([...byDim.entries()].map(([dim, rows]) => [`Q${dim}`, rows.length])),
    q9Summary: q9Summary.ndjson,
  }, null, 2));
}

await main();
