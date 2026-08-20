/* ============================================================================
 * NexxoN OS - Ultra-lightweight JavaScript interpreter  (v1.0)
 * ----------------------------------------------------------------------------
 * A toy JS engine with an explicit, well-defined subset that covers the
 * majority of inline <script> blocks on plain HTML pages:
 *
 *   * Lexer: identifiers, numbers (int + float), single/double-quoted
 *     strings, +, -, *, /, %, =, ==, !=, <, >, <=, >=, &&, ||, !,
 *     parentheses, braces, semicolons, commas.
 *   * Parser: variable declarations (var), if/else, while, function
 *     declarations, function calls, return.
 *   * Runtime: tagged-value type system (number, string, boolean,
 *     function, object, null/undefined).  Variables stored in a single
 *     global table; closures are not supported (yet).
 *   * Built-ins: console.log, document.title, document.getElementById,
 *     element.innerHTML write-through that bumps a re-layout flag on
 *     the dom_doc_t.
 *
 * Designed to evaluate <script> blocks once, on page load, with strict
 * caps on stack depth, value count and string size.  Out-of-budget
 * execution surfaces as a runtime error and is logged to COM1 but does
 * not crash the kernel.
 * ============================================================================ */
#ifndef NEXXON_JS_H
#define NEXXON_JS_H

#include "types.h"
#include "dom.h"

#define JS_MAX_VARS       64
#define JS_MAX_STR        128
#define JS_MAX_STACK      32
#define JS_MAX_FUNCS      16

typedef enum {
    JS_T_UNDEF = 0,
    JS_T_NULL,
    JS_T_NUM,
    JS_T_STR,
    JS_T_BOOL,
    JS_T_FUNC,
    JS_T_DOM_NODE,
} js_type_t;

typedef struct {
    js_type_t type;
    int32_t   num;
    char      str[JS_MAX_STR];
    int       func_idx;             /* index in vm->funcs                 */
    int       node_idx;             /* dom_doc_t node index               */
} js_value_t;

typedef struct {
    char         name[32];
    js_value_t   value;
} js_var_t;

typedef struct {
    char        name[32];
    int         body_start;
    int         body_end;
} js_func_t;

typedef struct {
    js_var_t    vars[JS_MAX_VARS];
    int         n_vars;
    js_func_t   funcs[JS_MAX_FUNCS];
    int         n_funcs;
    dom_doc_t  *dom;
    char        log[1024];
    int         log_len;
    bool        dom_dirty;          /* set when innerHTML / title changed */
    bool        error;
    char        error_msg[80];
} js_vm_t;

void js_init       (js_vm_t *vm, dom_doc_t *dom);
int  js_eval       (js_vm_t *vm, const char *src);
const char *js_log (const js_vm_t *vm);

#endif /* NEXXON_JS_H */
