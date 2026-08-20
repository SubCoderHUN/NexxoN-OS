/* ============================================================================
 * NexxoN OS - NXScript: TempleOS-flavoured, compact-syntax mini language
 * ----------------------------------------------------------------------------
 *  Keyword map (lexer translates these to standard tokens internally):
 *
 *     num     -> integer  (int)         pick    -> switch
 *     txt     -> string   (char *)      loop    -> while
 *     numdec  -> double   (float)       step    -> for
 *     bit     -> boolean  (true/false)  stop    -> break
 *                                       next    -> continue
 *     task    -> void / function decl   out     -> return
 *     none    -> NULL                   if/else -> unchanged
 *
 *  Builtins:
 *     print(x)           - render to the active script window (or shell)
 *     CreateWindow(x,y,w,h,"title") - open a WM window, set as output context
 *     WindowColor(argb)  - fill script window background with ARGB colour
 *     TextColor(argb)    - set subsequent print() foreground colour
 *
 *  Pre-defined colour constants (ARGB):
 *     RED  GREEN  BLUE  YELLOW  CYAN  WHITE  BLACK  NAVY
 *
 *  Storage:
 *     Up to NX_MAX_VARS named variables in a single global scope.
 *     Up to NX_MAX_TASKS user-defined `task` functions.
 *     The interpreter is tree-walking; no JIT yet, but the AST + value
 *     plumbing is ready for one (every node carries kind + operands).
 *
 *  Usage from the shell:
 *     nxscript_eval(line);
 *     // or for a multi-line block:
 *     nxscript_eval("task main(){ num x=5; print(x); out x; }  main();");
 * ============================================================================ */
#ifndef NEXXON_NXSCRIPT_H
#define NEXXON_NXSCRIPT_H

#include "types.h"

#define NX_OK              0
#define NX_ERR_LEX        -1
#define NX_ERR_PARSE      -2
#define NX_ERR_RUNTIME    -3
#define NX_ERR_LIMITS     -4
#define NX_ERR_TYPE       -5

void nxscript_init(void);

/* Evaluate one chunk of NXScript source.  Any output (print) flows
 * through term_putc().  Returns one of the NX_* status codes; on error
 * the formatted explanation is in `nxscript_last_error()`. */
int  nxscript_eval(const char *source);

const char *nxscript_last_error(void);

/* Reset the global scope: forget every user variable and task.  Useful
 * before re-running a clean script.  Builtins are unaffected. */
void nxscript_reset(void);

/* Heuristic the shell uses to decide whether a typed line should be
 * dispatched to NXScript instead of treated as a command. */
bool nxscript_looks_like(const char *line);

#endif /* NEXXON_NXSCRIPT_H */
