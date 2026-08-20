/* ============================================================================
 * NexxoN OS - Ultra-lightweight JavaScript interpreter
 * ----------------------------------------------------------------------------
 * Recursive-descent tree-walking interpreter.  Tokens are lexed lazily
 * from the input string; the parser is single-pass (no AST) which keeps
 * everything fitting in BSS without a heap.  Suitable for evaluating
 * the small inline <script> blocks that real web pages still rely on
 * for legacy reasons (Google Analytics-style banner snippets etc.).
 *
 * Statements supported:
 *   var <ident> = <expr>;
 *   if (<expr>) { ... } [else { ... }]
 *   while (<expr>) { ... }
 *   function <ident>(...) { ... }
 *   return <expr>;
 *   <expr>;
 *
 * Expressions:
 *   numeric literals, string literals, identifiers
 *   binary operators: + - * / %  ==  !=  <  >  <=  >=  &&  ||
 *   unary: -  !
 *   function calls
 *   property access (document.title, document.getElementById(...))
 *
 * Numbers are 32-bit signed integers.  String concatenation works via
 * the '+' operator when either operand is a string.
 * ============================================================================ */
#include "js.h"
#include "string.h"
#include "debug.h"

/* ---------- Lexer -------------------------------------------------------- */
typedef enum {
    TK_EOF = 0,
    TK_NUM, TK_STR, TK_IDENT,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_ASSIGN, TK_EQ, TK_NEQ, TK_LT, TK_GT, TK_LE, TK_GE,
    TK_AND, TK_OR, TK_NOT,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE,
    TK_SEMI, TK_COMMA, TK_DOT,
    TK_KW_VAR, TK_KW_IF, TK_KW_ELSE, TK_KW_WHILE, TK_KW_FUNC,
    TK_KW_RETURN, TK_KW_TRUE, TK_KW_FALSE, TK_KW_NULL, TK_KW_UNDEF,
} js_tok_t;

typedef struct {
    js_tok_t tok;
    int      num;
    char     str[JS_MAX_STR];
    int      pos;
} js_lex_t;

static void skip_ws(const char *src, int *p) {
    while (src[*p]) {
        char c = src[*p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { (*p)++; continue; }
        if (c == '/' && src[*p + 1] == '/') {
            while (src[*p] && src[*p] != '\n') (*p)++;
            continue;
        }
        if (c == '/' && src[*p + 1] == '*') {
            (*p) += 2;
            while (src[*p] && !(src[*p] == '*' && src[*p + 1] == '/')) (*p)++;
            if (src[*p]) (*p) += 2;
            continue;
        }
        break;
    }
}

static js_tok_t lex_next(const char *src, js_lex_t *l) {
    skip_ws(src, &l->pos);
    int p = l->pos;
    char c = src[p];
    if (!c) return l->tok = TK_EOF;
    if ((c >= '0' && c <= '9')) {
        int v = 0;
        while (src[p] >= '0' && src[p] <= '9') {
            v = v * 10 + (src[p++] - '0');
        }
        l->pos = p; l->num = v;
        return l->tok = TK_NUM;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$') {
        int n = 0;
        while ((src[p] >= 'a' && src[p] <= 'z') ||
               (src[p] >= 'A' && src[p] <= 'Z') ||
               (src[p] >= '0' && src[p] <= '9') ||
               src[p] == '_' || src[p] == '$') {
            if (n < JS_MAX_STR - 1) l->str[n++] = src[p];
            p++;
        }
        l->str[n] = 0;
        l->pos = p;
        if (!strcmp(l->str, "var"))      return l->tok = TK_KW_VAR;
        if (!strcmp(l->str, "if"))       return l->tok = TK_KW_IF;
        if (!strcmp(l->str, "else"))     return l->tok = TK_KW_ELSE;
        if (!strcmp(l->str, "while"))    return l->tok = TK_KW_WHILE;
        if (!strcmp(l->str, "function")) return l->tok = TK_KW_FUNC;
        if (!strcmp(l->str, "return"))   return l->tok = TK_KW_RETURN;
        if (!strcmp(l->str, "true"))     return l->tok = TK_KW_TRUE;
        if (!strcmp(l->str, "false"))    return l->tok = TK_KW_FALSE;
        if (!strcmp(l->str, "null"))     return l->tok = TK_KW_NULL;
        if (!strcmp(l->str, "undefined"))return l->tok = TK_KW_UNDEF;
        return l->tok = TK_IDENT;
    }
    if (c == '"' || c == '\'') {
        char q = c; p++;
        int n = 0;
        while (src[p] && src[p] != q && n < JS_MAX_STR - 1) {
            if (src[p] == '\\' && src[p + 1]) {
                char nx = src[p + 1];
                if (nx == 'n') l->str[n++] = '\n';
                else if (nx == 't') l->str[n++] = '\t';
                else l->str[n++] = nx;
                p += 2;
            } else {
                l->str[n++] = src[p++];
            }
        }
        l->str[n] = 0;
        if (src[p] == q) p++;
        l->pos = p;
        return l->tok = TK_STR;
    }
    p++;
    switch (c) {
        case '+': l->pos = p; return l->tok = TK_PLUS;
        case '-': l->pos = p; return l->tok = TK_MINUS;
        case '*': l->pos = p; return l->tok = TK_STAR;
        case '/': l->pos = p; return l->tok = TK_SLASH;
        case '%': l->pos = p; return l->tok = TK_PERCENT;
        case '(': l->pos = p; return l->tok = TK_LPAREN;
        case ')': l->pos = p; return l->tok = TK_RPAREN;
        case '{': l->pos = p; return l->tok = TK_LBRACE;
        case '}': l->pos = p; return l->tok = TK_RBRACE;
        case ';': l->pos = p; return l->tok = TK_SEMI;
        case ',': l->pos = p; return l->tok = TK_COMMA;
        case '.': l->pos = p; return l->tok = TK_DOT;
        case '=':
            if (src[p] == '=') { l->pos = p + 1; return l->tok = TK_EQ; }
            l->pos = p; return l->tok = TK_ASSIGN;
        case '!':
            if (src[p] == '=') { l->pos = p + 1; return l->tok = TK_NEQ; }
            l->pos = p; return l->tok = TK_NOT;
        case '<':
            if (src[p] == '=') { l->pos = p + 1; return l->tok = TK_LE; }
            l->pos = p; return l->tok = TK_LT;
        case '>':
            if (src[p] == '=') { l->pos = p + 1; return l->tok = TK_GE; }
            l->pos = p; return l->tok = TK_GT;
        case '&':
            if (src[p] == '&') { l->pos = p + 1; return l->tok = TK_AND; }
            break;
        case '|':
            if (src[p] == '|') { l->pos = p + 1; return l->tok = TK_OR; }
            break;
    }
    l->pos = p;
    return l->tok = TK_EOF;
}

/* ---------- Parser / evaluator ------------------------------------------- */
static js_value_t parse_expr(js_vm_t *vm, const char *src, js_lex_t *lex);
static void parse_block(js_vm_t *vm, const char *src, js_lex_t *lex);

static void vm_error(js_vm_t *vm, const char *msg) {
    vm->error = true;
    int n = 0;
    while (msg[n] && n < (int)sizeof(vm->error_msg) - 1) {
        vm->error_msg[n] = msg[n]; n++;
    }
    vm->error_msg[n] = 0;
    debug_printf("[js] error: %s\n", msg);
}

static void vm_log_append(js_vm_t *vm, const char *s) {
    while (*s && vm->log_len < (int)sizeof(vm->log) - 1) {
        vm->log[vm->log_len++] = *s++;
    }
    vm->log[vm->log_len] = 0;
}

static js_value_t v_num(int n)  { js_value_t v; memset(&v, 0, sizeof(v)); v.type = JS_T_NUM; v.num = n; return v; }
static js_value_t v_bool(int b) { js_value_t v; memset(&v, 0, sizeof(v)); v.type = JS_T_BOOL; v.num = b ? 1 : 0; return v; }
static js_value_t v_undef(void) { js_value_t v; memset(&v, 0, sizeof(v)); v.type = JS_T_UNDEF; return v; }
static js_value_t v_str(const char *s) {
    js_value_t v; memset(&v, 0, sizeof(v));
    v.type = JS_T_STR;
    int n = 0;
    while (s[n] && n < JS_MAX_STR - 1) { v.str[n] = s[n]; n++; }
    v.str[n] = 0;
    return v;
}

static js_var_t *vm_lookup(js_vm_t *vm, const char *name) {
    for (int i = 0; i < vm->n_vars; i++) {
        if (strcmp(vm->vars[i].name, name) == 0) return &vm->vars[i];
    }
    return NULL;
}

static js_var_t *vm_set(js_vm_t *vm, const char *name, js_value_t v) {
    js_var_t *p = vm_lookup(vm, name);
    if (!p) {
        if (vm->n_vars >= JS_MAX_VARS) { vm_error(vm, "var table full"); return NULL; }
        p = &vm->vars[vm->n_vars++];
        int n = 0; while (name[n] && n < 31) { p->name[n] = name[n]; n++; }
        p->name[n] = 0;
    }
    p->value = v;
    return p;
}

static int to_int(js_value_t v) {
    if (v.type == JS_T_NUM)  return v.num;
    if (v.type == JS_T_BOOL) return v.num;
    if (v.type == JS_T_STR) {
        int n = 0, i = 0;
        if (v.str[0] == '-') i = 1;
        while (v.str[i] >= '0' && v.str[i] <= '9') {
            n = n * 10 + (v.str[i++] - '0');
        }
        return v.str[0] == '-' ? -n : n;
    }
    return 0;
}

static void value_to_str(js_value_t v, char *out, int cap) {
    if (v.type == JS_T_STR) {
        int n = 0;
        while (v.str[n] && n < cap - 1) { out[n] = v.str[n]; n++; }
        out[n] = 0;
        return;
    }
    if (v.type == JS_T_NUM) {
        ksnprintf(out, cap, "%d", v.num);
        return;
    }
    if (v.type == JS_T_BOOL) {
        ksnprintf(out, cap, v.num ? "true" : "false");
        return;
    }
    if (v.type == JS_T_NULL)   { ksnprintf(out, cap, "null"); return; }
    out[0] = 0;
}

/* Primary expression: literal / identifier / parenthesised. */
static js_value_t parse_primary(js_vm_t *vm, const char *src, js_lex_t *lex) {
    if (vm->error) return v_undef();
    js_tok_t t = lex->tok;
    if (t == TK_NUM) {
        js_value_t v = v_num(lex->num);
        lex_next(src, lex);
        return v;
    }
    if (t == TK_STR) {
        js_value_t v = v_str(lex->str);
        lex_next(src, lex);
        return v;
    }
    if (t == TK_KW_TRUE)  { lex_next(src, lex); return v_bool(1); }
    if (t == TK_KW_FALSE) { lex_next(src, lex); return v_bool(0); }
    if (t == TK_KW_NULL)  { lex_next(src, lex); js_value_t v = v_undef(); v.type = JS_T_NULL; return v; }
    if (t == TK_LPAREN) {
        lex_next(src, lex);
        js_value_t v = parse_expr(vm, src, lex);
        if (lex->tok != TK_RPAREN) { vm_error(vm, "expected ')'"); return v_undef(); }
        lex_next(src, lex);
        return v;
    }
    if (t == TK_IDENT) {
        char name[32];
        strncpy(name, lex->str, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        lex_next(src, lex);
        /* Property access chain like document.title or
         * document.getElementById("foo"). */
        if (lex->tok == TK_DOT) {
            lex_next(src, lex);
            if (lex->tok != TK_IDENT) {
                vm_error(vm, "expected property name after '.'");
                return v_undef();
            }
            char prop[32];
            strncpy(prop, lex->str, sizeof(prop) - 1);
            prop[sizeof(prop) - 1] = 0;
            lex_next(src, lex);
            if (strcmp(name, "console") == 0 && strcmp(prop, "log") == 0) {
                /* console.log(arg, ...); */
                if (lex->tok != TK_LPAREN) { vm_error(vm, "expected '(' after log"); return v_undef(); }
                lex_next(src, lex);
                while (lex->tok != TK_RPAREN && lex->tok != TK_EOF) {
                    js_value_t arg = parse_expr(vm, src, lex);
                    char buf[160];
                    value_to_str(arg, buf, sizeof(buf));
                    vm_log_append(vm, buf);
                    if (lex->tok == TK_COMMA) { vm_log_append(vm, " "); lex_next(src, lex); }
                }
                if (lex->tok == TK_RPAREN) lex_next(src, lex);
                vm_log_append(vm, "\n");
                return v_undef();
            }
            if (strcmp(name, "document") == 0) {
                if (strcmp(prop, "title") == 0) {
                    if (lex->tok == TK_ASSIGN) {
                        lex_next(src, lex);
                        js_value_t v = parse_expr(vm, src, lex);
                        char buf[80];
                        value_to_str(v, buf, sizeof(buf));
                        if (vm->dom && vm->dom->root >= 0) {
                            /* Locate first <title> node (the head walker
                             * leaves one if present), else create one as
                             * a sibling of <html>. */
                            for (int i = 0; i < vm->dom->used; i++) {
                                dom_node_t *n = &vm->dom->nodes[i];
                                if (n->in_use &&
                                    strcmp(n->tag, "title") == 0) {
                                    /* The title text is the first text
                                     * child, if any. */
                                    if (n->first_child >= 0) {
                                        strncpy(vm->dom->nodes[n->first_child].text,
                                                buf, DOM_TEXT_MAX - 1);
                                    }
                                    break;
                                }
                            }
                        }
                        return v;
                    }
                    /* read */
                    if (vm->dom) {
                        for (int i = 0; i < vm->dom->used; i++) {
                            dom_node_t *n = &vm->dom->nodes[i];
                            if (n->in_use && strcmp(n->tag, "title") == 0 &&
                                n->first_child >= 0) {
                                return v_str(vm->dom->nodes[n->first_child].text);
                            }
                        }
                    }
                    return v_str("");
                }
                if (strcmp(prop, "getElementById") == 0) {
                    if (lex->tok != TK_LPAREN) { vm_error(vm, "expected '('"); return v_undef(); }
                    lex_next(src, lex);
                    js_value_t arg = parse_expr(vm, src, lex);
                    if (lex->tok == TK_RPAREN) lex_next(src, lex);
                    char id[32];
                    value_to_str(arg, id, sizeof(id));
                    if (vm->dom) {
                        for (int i = 0; i < vm->dom->used; i++) {
                            const char *a = dom_attr(&vm->dom->nodes[i], "id");
                            if (a && strcmp(a, id) == 0) {
                                js_value_t v = v_undef();
                                v.type = JS_T_DOM_NODE;
                                v.node_idx = i;
                                return v;
                            }
                        }
                    }
                    js_value_t v = v_undef(); v.type = JS_T_NULL; return v;
                }
            }
            /* Generic: not implemented. */
            return v_undef();
        }
        /* Function call. */
        if (lex->tok == TK_LPAREN) {
            /* Built-in: parseInt(x), Number(x). */
            lex_next(src, lex);
            js_value_t args[4];
            int n_args = 0;
            while (lex->tok != TK_RPAREN && lex->tok != TK_EOF && n_args < 4) {
                args[n_args++] = parse_expr(vm, src, lex);
                if (lex->tok == TK_COMMA) lex_next(src, lex);
            }
            if (lex->tok == TK_RPAREN) lex_next(src, lex);
            if (strcmp(name, "parseInt") == 0 && n_args >= 1) {
                return v_num(to_int(args[0]));
            }
            /* Unknown user function: invoke if registered. */
            for (int fi = 0; fi < vm->n_funcs; fi++) {
                if (strcmp(vm->funcs[fi].name, name) == 0) {
                    /* Execute body in current scope (no closures). */
                    js_lex_t saved = *lex;
                    lex->pos = vm->funcs[fi].body_start;
                    lex_next(src, lex);
                    parse_block(vm, src, lex);
                    *lex = saved;
                    return v_undef();
                }
            }
            return v_undef();
        }
        /* Variable read. */
        js_var_t *p = vm_lookup(vm, name);
        if (p) return p->value;
        return v_undef();
    }
    if (t == TK_MINUS) {
        lex_next(src, lex);
        js_value_t r = parse_primary(vm, src, lex);
        return v_num(-to_int(r));
    }
    if (t == TK_NOT) {
        lex_next(src, lex);
        js_value_t r = parse_primary(vm, src, lex);
        return v_bool(!to_int(r));
    }
    return v_undef();
}

static js_value_t parse_mul(js_vm_t *vm, const char *src, js_lex_t *lex) {
    js_value_t l = parse_primary(vm, src, lex);
    while (lex->tok == TK_STAR || lex->tok == TK_SLASH || lex->tok == TK_PERCENT) {
        js_tok_t op = lex->tok;
        lex_next(src, lex);
        js_value_t r = parse_primary(vm, src, lex);
        int a = to_int(l), b = to_int(r);
        if (op == TK_STAR)    l = v_num(a * b);
        if (op == TK_SLASH)   l = v_num(b ? a / b : 0);
        if (op == TK_PERCENT) l = v_num(b ? a % b : 0);
    }
    return l;
}

static js_value_t parse_add(js_vm_t *vm, const char *src, js_lex_t *lex) {
    js_value_t l = parse_mul(vm, src, lex);
    while (lex->tok == TK_PLUS || lex->tok == TK_MINUS) {
        js_tok_t op = lex->tok;
        lex_next(src, lex);
        js_value_t r = parse_mul(vm, src, lex);
        if (op == TK_PLUS &&
            (l.type == JS_T_STR || r.type == JS_T_STR)) {
            char a[JS_MAX_STR], b[JS_MAX_STR], out[JS_MAX_STR];
            value_to_str(l, a, sizeof(a));
            value_to_str(r, b, sizeof(b));
            int ap = 0;
            while (a[ap] && ap < JS_MAX_STR - 1) { out[ap] = a[ap]; ap++; }
            int bp = 0;
            while (b[bp] && ap < JS_MAX_STR - 1) { out[ap++] = b[bp++]; }
            out[ap] = 0;
            l = v_str(out);
        } else {
            int a = to_int(l), b = to_int(r);
            l = v_num(op == TK_PLUS ? a + b : a - b);
        }
    }
    return l;
}

static js_value_t parse_cmp(js_vm_t *vm, const char *src, js_lex_t *lex) {
    js_value_t l = parse_add(vm, src, lex);
    while (lex->tok == TK_EQ || lex->tok == TK_NEQ ||
           lex->tok == TK_LT || lex->tok == TK_GT ||
           lex->tok == TK_LE || lex->tok == TK_GE) {
        js_tok_t op = lex->tok;
        lex_next(src, lex);
        js_value_t r = parse_add(vm, src, lex);
        int a = to_int(l), b = to_int(r);
        switch (op) {
            case TK_EQ:  l = v_bool(a == b); break;
            case TK_NEQ: l = v_bool(a != b); break;
            case TK_LT:  l = v_bool(a <  b); break;
            case TK_GT:  l = v_bool(a >  b); break;
            case TK_LE:  l = v_bool(a <= b); break;
            case TK_GE:  l = v_bool(a >= b); break;
            default: break;
        }
    }
    return l;
}

static js_value_t parse_logic(js_vm_t *vm, const char *src, js_lex_t *lex) {
    js_value_t l = parse_cmp(vm, src, lex);
    while (lex->tok == TK_AND || lex->tok == TK_OR) {
        js_tok_t op = lex->tok;
        lex_next(src, lex);
        js_value_t r = parse_cmp(vm, src, lex);
        l = v_bool(op == TK_AND ? (to_int(l) && to_int(r))
                                  : (to_int(l) || to_int(r)));
    }
    return l;
}

static js_value_t parse_expr(js_vm_t *vm, const char *src, js_lex_t *lex) {
    return parse_logic(vm, src, lex);
}

static void parse_stmt(js_vm_t *vm, const char *src, js_lex_t *lex);

static void parse_block(js_vm_t *vm, const char *src, js_lex_t *lex) {
    if (lex->tok != TK_LBRACE) {
        parse_stmt(vm, src, lex);
        return;
    }
    lex_next(src, lex);
    while (lex->tok != TK_RBRACE && lex->tok != TK_EOF && !vm->error) {
        parse_stmt(vm, src, lex);
    }
    if (lex->tok == TK_RBRACE) lex_next(src, lex);
}

static void parse_stmt(js_vm_t *vm, const char *src, js_lex_t *lex) {
    if (vm->error) return;
    if (lex->tok == TK_KW_VAR) {
        lex_next(src, lex);
        if (lex->tok != TK_IDENT) { vm_error(vm, "expected identifier after var"); return; }
        char name[32];
        strncpy(name, lex->str, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        lex_next(src, lex);
        js_value_t val = v_undef();
        if (lex->tok == TK_ASSIGN) {
            lex_next(src, lex);
            val = parse_expr(vm, src, lex);
        }
        vm_set(vm, name, val);
        if (lex->tok == TK_SEMI) lex_next(src, lex);
        return;
    }
    if (lex->tok == TK_KW_IF) {
        lex_next(src, lex);
        if (lex->tok != TK_LPAREN) { vm_error(vm, "expected '(' after if"); return; }
        lex_next(src, lex);
        js_value_t cond = parse_expr(vm, src, lex);
        if (lex->tok != TK_RPAREN) { vm_error(vm, "expected ')' after if-cond"); return; }
        lex_next(src, lex);
        if (to_int(cond)) {
            parse_block(vm, src, lex);
            if (lex->tok == TK_KW_ELSE) {
                /* Skip the else block. */
                lex_next(src, lex);
                if (lex->tok == TK_LBRACE) {
                    int depth = 1;
                    lex_next(src, lex);
                    while (depth > 0 && lex->tok != TK_EOF) {
                        if (lex->tok == TK_LBRACE) depth++;
                        if (lex->tok == TK_RBRACE) depth--;
                        if (depth > 0) lex_next(src, lex);
                    }
                    if (lex->tok == TK_RBRACE) lex_next(src, lex);
                } else {
                    parse_stmt(vm, src, lex);
                }
            }
        } else {
            /* Skip the then block. */
            if (lex->tok == TK_LBRACE) {
                int depth = 1;
                lex_next(src, lex);
                while (depth > 0 && lex->tok != TK_EOF) {
                    if (lex->tok == TK_LBRACE) depth++;
                    if (lex->tok == TK_RBRACE) depth--;
                    if (depth > 0) lex_next(src, lex);
                }
                if (lex->tok == TK_RBRACE) lex_next(src, lex);
            } else {
                parse_stmt(vm, src, lex);
            }
            if (lex->tok == TK_KW_ELSE) {
                lex_next(src, lex);
                parse_block(vm, src, lex);
            }
        }
        return;
    }
    if (lex->tok == TK_KW_WHILE) {
        lex_next(src, lex);
        if (lex->tok != TK_LPAREN) { vm_error(vm, "expected '(' after while"); return; }
        int cond_pos = lex->pos;
        /* Loop body start is parsed multiple times. */
        for (int it = 0; it < 1000; it++) {
            lex->pos = cond_pos;
            lex_next(src, lex);
            js_value_t cond = parse_expr(vm, src, lex);
            if (lex->tok != TK_RPAREN) { vm_error(vm, "expected ')' in while"); return; }
            lex_next(src, lex);
            if (!to_int(cond)) {
                /* Skip body. */
                if (lex->tok == TK_LBRACE) {
                    int depth = 1;
                    lex_next(src, lex);
                    while (depth > 0 && lex->tok != TK_EOF) {
                        if (lex->tok == TK_LBRACE) depth++;
                        if (lex->tok == TK_RBRACE) depth--;
                        if (depth > 0) lex_next(src, lex);
                    }
                    if (lex->tok == TK_RBRACE) lex_next(src, lex);
                }
                return;
            }
            parse_block(vm, src, lex);
            if (vm->error) return;
        }
        vm_error(vm, "while loop exceeded 1000 iterations");
        return;
    }
    if (lex->tok == TK_KW_FUNC) {
        lex_next(src, lex);
        if (lex->tok != TK_IDENT) { vm_error(vm, "expected function name"); return; }
        char name[32];
        strncpy(name, lex->str, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        lex_next(src, lex);
        /* Skip the param list. */
        if (lex->tok == TK_LPAREN) {
            int depth = 1;
            lex_next(src, lex);
            while (depth > 0 && lex->tok != TK_EOF) {
                if (lex->tok == TK_LPAREN) depth++;
                if (lex->tok == TK_RPAREN) depth--;
                if (depth > 0) lex_next(src, lex);
            }
            if (lex->tok == TK_RPAREN) lex_next(src, lex);
        }
        if (lex->tok != TK_LBRACE) { vm_error(vm, "expected '{' for function body"); return; }
        if (vm->n_funcs >= JS_MAX_FUNCS) { vm_error(vm, "function table full"); return; }
        js_func_t *f = &vm->funcs[vm->n_funcs++];
        strncpy(f->name, name, sizeof(f->name) - 1);
        f->name[sizeof(f->name) - 1] = 0;
        f->body_start = lex->pos - 1;
        /* Skip the body. */
        int depth = 1;
        lex_next(src, lex);
        while (depth > 0 && lex->tok != TK_EOF) {
            if (lex->tok == TK_LBRACE) depth++;
            if (lex->tok == TK_RBRACE) depth--;
            if (depth > 0) lex_next(src, lex);
        }
        f->body_end = lex->pos;
        if (lex->tok == TK_RBRACE) lex_next(src, lex);
        return;
    }
    if (lex->tok == TK_KW_RETURN) {
        lex_next(src, lex);
        if (lex->tok != TK_SEMI) (void)parse_expr(vm, src, lex);
        if (lex->tok == TK_SEMI) lex_next(src, lex);
        return;
    }
    /* Assignment or bare expression statement. */
    if (lex->tok == TK_IDENT) {
        char name[32];
        strncpy(name, lex->str, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        int saved = lex->pos;
        lex_next(src, lex);
        if (lex->tok == TK_ASSIGN) {
            lex_next(src, lex);
            js_value_t v = parse_expr(vm, src, lex);
            vm_set(vm, name, v);
            if (lex->tok == TK_SEMI) lex_next(src, lex);
            return;
        }
        /* Rewind and parse as bare expr. */
        lex->pos = saved;
        lex_next(src, lex);
    }
    (void)parse_expr(vm, src, lex);
    if (lex->tok == TK_SEMI) lex_next(src, lex);
}

void js_init(js_vm_t *vm, dom_doc_t *dom) {
    memset(vm, 0, sizeof(*vm));
    vm->dom = dom;
}

int js_eval(js_vm_t *vm, const char *src) {
    if (!vm || !src) return -1;
    js_lex_t lex; memset(&lex, 0, sizeof(lex)); lex.pos = 0;
    lex_next(src, &lex);
    while (lex.tok != TK_EOF && !vm->error) {
        parse_stmt(vm, src, &lex);
    }
    return vm->error ? -1 : 0;
}

const char *js_log(const js_vm_t *vm) { return vm ? vm->log : ""; }
