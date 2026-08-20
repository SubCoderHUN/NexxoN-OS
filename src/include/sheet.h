/* ============================================================================
 * NexxoN OS - Spreadsheet engine (nSheet)  (v1.0)
 * ----------------------------------------------------------------------------
 * Sparse matrix workbook + formula evaluator + .xlsx ingest/emit.
 *
 *   * Sparse storage: open-addressed hash on (row,col).  Empty cells
 *     consume zero memory.  Capacity 4096 live cells.
 *   * Cells carry either a literal value (string/number) or a raw
 *     formula string starting with '='.
 *   * Formula evaluator is a recursive-descent expression parser with
 *     range-aware builtins SUM, AVERAGE, MIN, MAX, COUNT.  Identifier
 *     lookup is case-insensitive so SZUM (Hungarian SUM) maps onto the
 *     same function symbol when the global language is HU.
 *   * .xlsx ingest pipelines through deflate_inflate + a SAX-style XML
 *     parser that reads sharedStrings.xml + worksheets/sheet1.xml.
 *
 * Designed for productivity-scale workbooks (≤ 32 columns × ≤ 1024
 * rows); larger sheets either fail to load or get truncated.
 * ============================================================================ */
#ifndef NEXXON_SHEET_H
#define NEXXON_SHEET_H

#include "types.h"

#define SHEET_MAX_CELLS    4096
#define SHEET_FORMULA_MAX  128
#define SHEET_STR_MAX      64
#define SHEET_MAX_COLS     32
#define SHEET_MAX_ROWS     1024

typedef enum {
    CELL_EMPTY   = 0,
    CELL_NUMBER  = 1,
    CELL_STRING  = 2,
    CELL_FORMULA = 3,
    CELL_ERROR   = 4,
} cell_type_t;

typedef struct {
    int          row;
    int          col;
    cell_type_t  type;
    double       value;          /* numeric result of formula            */
    char         text[SHEET_STR_MAX];
    char         formula[SHEET_FORMULA_MAX];
} sheet_cell_t;

typedef struct {
    int          n;
    sheet_cell_t cells[SHEET_MAX_CELLS];
} sheet_t;

void sheet_init     (sheet_t *s);
sheet_cell_t *sheet_cell  (sheet_t *s, int row, int col);
sheet_cell_t *sheet_at    (sheet_t *s, int row, int col); /* read-only lookup */

int  sheet_set_number  (sheet_t *s, int row, int col, double v);
int  sheet_set_string  (sheet_t *s, int row, int col, const char *str);
int  sheet_set_formula (sheet_t *s, int row, int col, const char *formula);

/* Evaluate every formula cell.  Iterates twice so a -> b -> a chains
 * settle.  Cycles return CELL_ERROR with value 0. */
void sheet_recalc      (sheet_t *s);

/* Load a workbook from a .xlsx blob in memory.  Returns 0 on success. */
int  sheet_load_xlsx   (sheet_t *s, const uint8_t *blob, uint32_t blob_len);

/* Serialise the workbook back to .xlsx (deflate-compressed). */
int  sheet_save_xlsx   (const sheet_t *s, uint8_t *out, uint32_t out_cap);

/* Reference parser: "A1" -> row=0 col=0, "BC42" -> row=41 col=54. */
int  sheet_parse_ref   (const char *ref, int *row, int *col);

#endif /* NEXXON_SHEET_H */
