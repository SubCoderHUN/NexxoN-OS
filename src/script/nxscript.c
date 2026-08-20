/* ============================================================================
 * NexxoN OS - NXScript: lexer + parser + tree-walking interpreter
 * ----------------------------------------------------------------------------
 *
 * Pipeline:
 *
 *      source -> lex() -> token[]
 *                       -> parse_program() -> AST node[]
 *                                          -> exec_block(root) -> output
 *
 * No malloc.  Every dynamic structure (tokens, AST nodes, variables) is
 * carved out of fixed BSS arrays.  Each call to nxscript_eval() resets
 * the lexer/parser pools but PRESERVES the variable + task tables so
 * subsequent prompts can see prior definitions.
 *
 * Custom keyword translation happens in `match_keyword()` - everywhere
 * else in the file we speak the canonical token kinds (TK_INT, TK_WHILE,
 * etc).  This keeps the parser look-and-feel familiar while still letting
 * the user type the shortened NXScript syntax.
 * ============================================================================ */
#include "nxscript.h"
#include "string.h"
#include "terminal.h"
#include "window.h"
#include "gfx.h"
#include "font.h"
#include "keyboard.h"

/* ====================================================================== */
/*                        Script window context                            */
/* ====================================================================== */
/* When a script calls CreateWindow(), the resulting window becomes the
 * "active script context".  Subsequent print() calls render into that
 * window's content buffer instead of the shell terminal.              */
static window_t *g_script_win        = NULL;
static uint32_t  g_script_win_color  = 0xFF000020;  /* dark navy default */
static uint32_t  g_script_text_color = 0xFFFFFFFF;  /* white default */
static int       g_script_text_y     = 4;            /* next output Y px */

/* ====================================================================== */
/*                            Configuration                                */
/* ====================================================================== */
#define NX_MAX_TOKENS     512
#define NX_MAX_NODES      512
#define NX_MAX_VARS        64
#define NX_MAX_TASKS       16
#define NX_MAX_ARGS         8
/* Kept small on purpose: every value_t lives on the stack inside exec_node,
 * and the tree-walking interpreter recurses one frame per AST sub-tree. */
#define NX_MAX_STR         64

/* ====================================================================== */
/*                              Token kinds                                */
/* ====================================================================== */
typedef enum {
    TK_EOF = 0,
    /* literals / identifier */
    TK_INT_LIT, TK_DEC_LIT, TK_STR_LIT, TK_IDENT,
    /* type keywords */
    TK_KW_INT, TK_KW_STR, TK_KW_DEC, TK_KW_BOOL,
    /* control keywords */
    TK_KW_IF, TK_KW_ELSE, TK_KW_WHILE, TK_KW_FOR, TK_KW_SWITCH,
    TK_KW_BREAK, TK_KW_CONTINUE, TK_KW_RETURN, TK_KW_VOID, TK_KW_NULL,
    /* boolean literals */
    TK_KW_TRUE, TK_KW_FALSE,
    /* punctuation */
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE,
    TK_COMMA, TK_SEMI, TK_DOT,
    /* operators */
    TK_ASSIGN, TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQ, TK_NE, TK_LT, TK_GT, TK_LE, TK_GE,
    TK_AND, TK_OR, TK_NOT,
} tok_kind_t;

typedef struct {
    tok_kind_t kind;
    int        line;
    int        ival;       /* TK_INT_LIT */
    double     dval;       /* TK_DEC_LIT */
    char       text[64];   /* TK_IDENT, TK_STR_LIT */
} tok_t;

static tok_t  g_toks[NX_MAX_TOKENS];
static int    g_tok_count;

/* ====================================================================== */
/*                                Lexer                                    */
/* ====================================================================== */
static const char *g_src;
static int         g_pos;
static int         g_line;
static char        g_err[NX_MAX_STR];

/* Public error format every NXScript caller sees: gives the user enough
 * context to find the typo without having to read the parser internals. */
static void set_err(int line, const char *fmt, ...) {
    char buf[NX_MAX_STR];
    va_list ap; va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (line > 0)
        ksnprintf(g_err, sizeof(g_err), "[NXScript Error] %s at line %d", buf, line);
    else
        ksnprintf(g_err, sizeof(g_err), "[NXScript Error] %s", buf);
}

static int peek_ch (void)        { return g_src[g_pos]; }
static int peek_ch2(void)        { return g_src[g_pos + 1]; }
static int next_ch (void)        { return g_src[g_pos++]; }

/* Translate NXScript keyword text -> canonical token kind. */
static int match_keyword(const char *s) {
    /* types */
    if (strcmp(s, "num")    == 0) return TK_KW_INT;
    if (strcmp(s, "txt")    == 0) return TK_KW_STR;
    if (strcmp(s, "numdec") == 0) return TK_KW_DEC;
    if (strcmp(s, "bit")    == 0) return TK_KW_BOOL;
    if (strcmp(s, "task")   == 0) return TK_KW_VOID;
    if (strcmp(s, "none")   == 0) return TK_KW_NULL;
    /* control */
    if (strcmp(s, "if")     == 0) return TK_KW_IF;
    if (strcmp(s, "else")   == 0) return TK_KW_ELSE;
    if (strcmp(s, "loop")   == 0) return TK_KW_WHILE;
    if (strcmp(s, "step")   == 0) return TK_KW_FOR;
    if (strcmp(s, "pick")   == 0) return TK_KW_SWITCH;
    if (strcmp(s, "stop")   == 0) return TK_KW_BREAK;
    if (strcmp(s, "next")   == 0) return TK_KW_CONTINUE;
    if (strcmp(s, "out")    == 0) return TK_KW_RETURN;
    /* literals */
    if (strcmp(s, "true")   == 0) return TK_KW_TRUE;
    if (strcmp(s, "false")  == 0) return TK_KW_FALSE;
    return -1;
}

static int lex(void) {
    g_tok_count = 0;
    g_line = 1;
    while (peek_ch()) {
        char c = (char)peek_ch();
        /* whitespace */
        if (c == ' ' || c == '\t' || c == '\r') { g_pos++; continue; }
        if (c == '\n') { g_pos++; g_line++; continue; }
        /* line comment */
        if (c == '/' && peek_ch2() == '/') {
            while (peek_ch() && peek_ch() != '\n') g_pos++;
            continue;
        }

        if (g_tok_count >= NX_MAX_TOKENS) {
            set_err(g_line, "too many tokens (max %d)", NX_MAX_TOKENS);
            return NX_ERR_LIMITS;
        }
        tok_t *t = &g_toks[g_tok_count];
        t->line = g_line;
        t->text[0] = 0;

        /* identifier or keyword */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            int i = 0;
            while (((char)peek_ch() >= 'a' && (char)peek_ch() <= 'z') ||
                   ((char)peek_ch() >= 'A' && (char)peek_ch() <= 'Z') ||
                   ((char)peek_ch() >= '0' && (char)peek_ch() <= '9') ||
                   (char)peek_ch() == '_') {
                if (i + 1 < (int)sizeof(t->text)) t->text[i++] = (char)next_ch();
                else next_ch();
            }
            t->text[i] = 0;
            int kw = match_keyword(t->text);
            t->kind = (tok_kind_t)(kw >= 0 ? kw : TK_IDENT);
            g_tok_count++;
            continue;
        }
        /* number */
        if (c >= '0' && c <= '9') {
            int v = 0;
            while ((char)peek_ch() >= '0' && (char)peek_ch() <= '9') {
                v = v * 10 + (next_ch() - '0');
            }
            if ((char)peek_ch() == '.') {
                next_ch();
                double f = 0, m = 0.1;
                while ((char)peek_ch() >= '0' && (char)peek_ch() <= '9') {
                    f += (next_ch() - '0') * m;
                    m *= 0.1;
                }
                t->kind = TK_DEC_LIT;
                t->dval = (double)v + f;
            } else {
                t->kind = TK_INT_LIT;
                t->ival = v;
            }
            g_tok_count++;
            continue;
        }
        /* string */
        if (c == '"') {
            next_ch();
            int i = 0;
            while (peek_ch() && (char)peek_ch() != '"') {
                char ch = (char)next_ch();
                if (ch == '\\' && peek_ch()) {
                    char esc = (char)next_ch();
                    switch (esc) {
                        case 'n': ch = '\n'; break;
                        case 't': ch = '\t'; break;
                        case '\\': ch = '\\'; break;
                        case '"': ch = '"';  break;
                        default:  ch = esc;  break;
                    }
                }
                if (i + 1 < (int)sizeof(t->text)) t->text[i++] = ch;
            }
            t->text[i] = 0;
            if ((char)peek_ch() != '"') { set_err(g_line, "unterminated string"); return NX_ERR_LEX; }
            next_ch();
            t->kind = TK_STR_LIT;
            g_tok_count++;
            continue;
        }
        /* punctuation / operators */
        #define ONE(c, k) case c: g_pos++; t->kind = (k); g_tok_count++; goto next_tok
        #define TWO(c2, k2, k1) \
            if (peek_ch2() == (c2)) { g_pos += 2; t->kind = (k2); g_tok_count++; goto next_tok; } \
            else { g_pos++; t->kind = (k1); g_tok_count++; goto next_tok; }
        switch (c) {
            ONE('(', TK_LPAREN);  ONE(')', TK_RPAREN);
            ONE('{', TK_LBRACE);  ONE('}', TK_RBRACE);
            ONE(',', TK_COMMA);   ONE(';', TK_SEMI);
            ONE('.', TK_DOT);
            ONE('+', TK_PLUS);    ONE('-', TK_MINUS);
            ONE('*', TK_STAR);    ONE('/', TK_SLASH);
            ONE('%', TK_PERCENT);
            case '=': TWO('=', TK_EQ, TK_ASSIGN);
            case '!': TWO('=', TK_NE, TK_NOT);
            case '<': TWO('=', TK_LE, TK_LT);
            case '>': TWO('=', TK_GE, TK_GT);
            case '&': if (peek_ch2() == '&') { g_pos += 2; t->kind = TK_AND; g_tok_count++; goto next_tok; }
                      set_err(g_line, "unexpected '&'"); return NX_ERR_LEX;
            case '|': if (peek_ch2() == '|') { g_pos += 2; t->kind = TK_OR;  g_tok_count++; goto next_tok; }
                      set_err(g_line, "unexpected '|'"); return NX_ERR_LEX;
        }
        #undef ONE
        #undef TWO
        set_err(g_line, "unexpected character '%c' (0x%x)", c, (uint32_t)c);
        return NX_ERR_LEX;
next_tok: ;
    }
    /* EOF sentinel */
    if (g_tok_count < NX_MAX_TOKENS) {
        g_toks[g_tok_count].kind = TK_EOF;
        g_toks[g_tok_count].line = g_line;
        g_tok_count++;
    }
    return NX_OK;
}

/* ====================================================================== */
/*                                  AST                                    */
/* ====================================================================== */
typedef enum {
    /* values */
    N_INT, N_DEC, N_STR, N_BOOL, N_NULL, N_IDENT,
    /* binary ops */
    N_BIN_ADD, N_BIN_SUB, N_BIN_MUL, N_BIN_DIV, N_BIN_MOD,
    N_BIN_EQ, N_BIN_NE, N_BIN_LT, N_BIN_GT, N_BIN_LE, N_BIN_GE,
    N_AND, N_OR,
    /* unary */
    N_NEG, N_NOT,
    /* misc */
    N_ASSIGN, N_CALL, N_VARDECL,
    /* statements */
    N_BLOCK, N_IF, N_WHILE, N_FOR, N_RETURN, N_BREAK, N_CONTINUE,
    N_TASK,     /* task name(args){block}            */
    N_EXPR_STMT,
} node_kind_t;

typedef struct node {
    node_kind_t  kind;
    int          line;       /* source line, propagated from the token that
                                produced this node - used by error reports  */
    int          ival;
    double       dval;
    char         text[64];
    /* children: up to 4 + an arglist */
    struct node *a, *b, *c, *d;
    struct node *args[NX_MAX_ARGS];
    int          nargs;
    struct node *next;       /* sibling chain for block statements */
} node_t;

static node_t g_nodes[NX_MAX_NODES];
static int    g_node_used;

/* `g_current_line` tracks "the line we were on when make_node() ran".  The
 * parser updates it from peek()->line so every AST node ends up annotated
 * even before we explicitly thread the token through, and the executor can
 * cite a line in error messages. */
static int g_current_line = 0;

static node_t *make_node(node_kind_t k) {
    if (g_node_used >= NX_MAX_NODES) return NULL;
    node_t *n = &g_nodes[g_node_used++];
    n->kind = k;
    n->line = g_current_line;
    n->ival = 0; n->dval = 0.0; n->text[0] = 0;
    n->a = n->b = n->c = n->d = NULL;
    n->nargs = 0;
    n->next = NULL;
    return n;
}

/* ====================================================================== */
/*                       Builtin function registry                          */
/* ====================================================================== */
/* Names known to the interpreter as functions.  Used both for error
 * reporting (e.g. "WindowColor used as a variable") and for the runtime
 * dispatch path so a single source-of-truth keeps the two in sync. */
typedef struct {
    const char *name;
    int         min_args;
    int         max_args;
} builtin_t;

static const builtin_t g_builtins[] = {
    { "print",        0, NX_MAX_ARGS },
    { "CreateWindow", 5, 5 },
    { "WindowColor",  1, 1 },
    { "TextColor",    1, 1 },
    { NULL, 0, 0 },
};

static const builtin_t *find_builtin(const char *name) {
    for (int i = 0; g_builtins[i].name; i++) {
        if (strcmp(g_builtins[i].name, name) == 0) return &g_builtins[i];
    }
    return NULL;
}

static bool is_builtin(const char *name) { return find_builtin(name) != NULL; }

/* ====================================================================== */
/*                                 Parser                                  */
/* ====================================================================== */
static int g_p;

static tok_t *peek(void) {
    /* Keep g_current_line in sync with the *next* token so make_node()
     * stamps the right line on whatever the parser is about to produce. */
    tok_t *t = &g_toks[g_p];
    g_current_line = t->line;
    return t;
}
static tok_t *advance(void)  { return &g_toks[g_p++]; }
static bool   check(tok_kind_t k) { return peek()->kind == k; }
static bool   accept(tok_kind_t k){ if (check(k)) { g_p++; return true; } return false; }
static int    expect(tok_kind_t k, const char *what) {
    if (!accept(k)) { set_err(peek()->line, "expected %s", what); return NX_ERR_PARSE; }
    return NX_OK;
}

/* Forward decls */
static node_t *parse_expr(void);
static node_t *parse_stmt(void);
static node_t *parse_block(void);

static node_t *parse_primary(void) {
    tok_t *t = peek();
    if (t->kind == TK_INT_LIT) {
        advance();
        node_t *n = make_node(N_INT);
        if (n) n->ival = t->ival;
        return n;
    }
    if (t->kind == TK_DEC_LIT) {
        advance();
        node_t *n = make_node(N_DEC);
        if (n) n->dval = t->dval;
        return n;
    }
    if (t->kind == TK_STR_LIT) {
        advance();
        node_t *n = make_node(N_STR);
        if (n) strncpy(n->text, t->text, sizeof(n->text) - 1);
        return n;
    }
    if (t->kind == TK_KW_TRUE)  { advance(); node_t *n = make_node(N_BOOL); if (n) n->ival = 1; return n; }
    if (t->kind == TK_KW_FALSE) { advance(); node_t *n = make_node(N_BOOL); if (n) n->ival = 0; return n; }
    if (t->kind == TK_KW_NULL)  { advance(); return make_node(N_NULL); }
    if (t->kind == TK_LPAREN) {
        advance();
        node_t *e = parse_expr();
        if (expect(TK_RPAREN, "')'") != NX_OK) return NULL;
        return e;
    }
    if (t->kind == TK_IDENT) {
        char name[64];
        strncpy(name, t->text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        advance();
        if (accept(TK_LPAREN)) {
            /* call */
            node_t *c = make_node(N_CALL);
            if (!c) return NULL;
            strncpy(c->text, name, sizeof(c->text) - 1);
            if (!check(TK_RPAREN)) {
                do {
                    if (c->nargs >= NX_MAX_ARGS) {
                        set_err(peek()->line, "too many call args");
                        return NULL;
                    }
                    c->args[c->nargs++] = parse_expr();
                } while (accept(TK_COMMA));
            }
            if (expect(TK_RPAREN, "')'") != NX_OK) return NULL;
            return c;
        }
        node_t *id = make_node(N_IDENT);
        if (id) strncpy(id->text, name, sizeof(id->text) - 1);
        return id;
    }
    set_err(t->line, "unexpected token in expression");
    return NULL;
}

static node_t *parse_unary(void) {
    if (accept(TK_MINUS)) {
        node_t *u = make_node(N_NEG);
        if (!u) return NULL;
        u->a = parse_unary();
        return u;
    }
    if (accept(TK_NOT)) {
        node_t *u = make_node(N_NOT);
        if (!u) return NULL;
        u->a = parse_unary();
        return u;
    }
    return parse_primary();
}

static node_t *parse_mul(void) {
    node_t *left = parse_unary();
    while (1) {
        node_kind_t op;
        if      (accept(TK_STAR))    op = N_BIN_MUL;
        else if (accept(TK_SLASH))   op = N_BIN_DIV;
        else if (accept(TK_PERCENT)) op = N_BIN_MOD;
        else break;
        node_t *n = make_node(op);
        if (!n) return NULL;
        n->a = left;
        n->b = parse_unary();
        left = n;
    }
    return left;
}

static node_t *parse_add(void) {
    node_t *left = parse_mul();
    while (1) {
        node_kind_t op;
        if      (accept(TK_PLUS))  op = N_BIN_ADD;
        else if (accept(TK_MINUS)) op = N_BIN_SUB;
        else break;
        node_t *n = make_node(op);
        if (!n) return NULL;
        n->a = left;
        n->b = parse_mul();
        left = n;
    }
    return left;
}

static node_t *parse_cmp(void) {
    node_t *left = parse_add();
    while (1) {
        node_kind_t op;
        if      (accept(TK_LT)) op = N_BIN_LT;
        else if (accept(TK_GT)) op = N_BIN_GT;
        else if (accept(TK_LE)) op = N_BIN_LE;
        else if (accept(TK_GE)) op = N_BIN_GE;
        else break;
        node_t *n = make_node(op);
        if (!n) return NULL;
        n->a = left;
        n->b = parse_add();
        left = n;
    }
    return left;
}

static node_t *parse_eq(void) {
    node_t *left = parse_cmp();
    while (1) {
        node_kind_t op;
        if      (accept(TK_EQ)) op = N_BIN_EQ;
        else if (accept(TK_NE)) op = N_BIN_NE;
        else break;
        node_t *n = make_node(op);
        if (!n) return NULL;
        n->a = left;
        n->b = parse_cmp();
        left = n;
    }
    return left;
}

static node_t *parse_and(void) {
    node_t *left = parse_eq();
    while (accept(TK_AND)) {
        node_t *n = make_node(N_AND);
        if (!n) return NULL;
        n->a = left;
        n->b = parse_eq();
        left = n;
    }
    return left;
}

static node_t *parse_or(void) {
    node_t *left = parse_and();
    while (accept(TK_OR)) {
        node_t *n = make_node(N_OR);
        if (!n) return NULL;
        n->a = left;
        n->b = parse_and();
        left = n;
    }
    return left;
}

static node_t *parse_assignment(void) {
    node_t *left = parse_or();
    if (accept(TK_ASSIGN)) {
        if (!left || left->kind != N_IDENT) {
            set_err(peek()->line, "left-hand side of '=' must be a variable");
            return NULL;
        }
        node_t *n = make_node(N_ASSIGN);
        if (!n) return NULL;
        strncpy(n->text, left->text, sizeof(n->text) - 1);
        n->a = parse_assignment();
        return n;
    }
    return left;
}

static node_t *parse_expr(void) { return parse_assignment(); }

/* type-keyword guard for var decls */
static bool is_type_kw(tok_kind_t k) {
    return k == TK_KW_INT || k == TK_KW_STR ||
           k == TK_KW_DEC || k == TK_KW_BOOL;
}

static node_t *parse_var_decl(void) {
    tok_t *type = advance();
    if (!check(TK_IDENT)) {
        set_err(peek()->line, "expected variable name after type");
        return NULL;
    }
    node_t *n = make_node(N_VARDECL);
    if (!n) return NULL;
    n->ival = (int)type->kind;
    tok_t *name = advance();
    strncpy(n->text, name->text, sizeof(n->text) - 1);
    if (accept(TK_ASSIGN)) {
        n->a = parse_expr();
    }
    if (expect(TK_SEMI, "';' after variable declaration") != NX_OK) return NULL;
    return n;
}

static node_t *parse_block(void) {
    if (expect(TK_LBRACE, "'{'") != NX_OK) return NULL;
    node_t *blk = make_node(N_BLOCK);
    if (!blk) return NULL;
    node_t *tail = NULL;
    while (!check(TK_RBRACE) && !check(TK_EOF)) {
        node_t *s = parse_stmt();
        if (!s) return NULL;
        if (!blk->a) blk->a = s;
        else         tail->next = s;
        tail = s;
    }
    if (expect(TK_RBRACE, "'}'") != NX_OK) return NULL;
    return blk;
}

static node_t *parse_task(void) {
    advance();                                   /* consume 'task' */
    if (!check(TK_IDENT)) {
        set_err(peek()->line, "expected task name after 'task'");
        return NULL;
    }
    node_t *t = make_node(N_TASK);
    if (!t) return NULL;
    tok_t *name = advance();
    strncpy(t->text, name->text, sizeof(t->text) - 1);
    if (expect(TK_LPAREN, "'(' after task name") != NX_OK) return NULL;
    /* optional parameters */
    if (!check(TK_RPAREN)) {
        do {
            if (t->nargs >= NX_MAX_ARGS) {
                set_err(peek()->line, "too many task params"); return NULL;
            }
            if (is_type_kw(peek()->kind)) advance();   /* type optional */
            if (!check(TK_IDENT)) {
                set_err(peek()->line, "expected param name"); return NULL;
            }
            node_t *p = make_node(N_IDENT);
            strncpy(p->text, advance()->text, sizeof(p->text) - 1);
            t->args[t->nargs++] = p;
        } while (accept(TK_COMMA));
    }
    if (expect(TK_RPAREN, "')'") != NX_OK) return NULL;
    t->a = parse_block();
    return t;
}

static node_t *parse_stmt(void) {
    tok_t *t = peek();

    if (t->kind == TK_KW_VOID)   return parse_task();
    if (is_type_kw(t->kind))     return parse_var_decl();
    if (t->kind == TK_LBRACE)    return parse_block();

    if (t->kind == TK_KW_IF) {
        advance();
        node_t *s = make_node(N_IF);
        if (!s) return NULL;
        if (expect(TK_LPAREN, "'('") != NX_OK) return NULL;
        s->a = parse_expr();
        if (expect(TK_RPAREN, "')'") != NX_OK) return NULL;
        s->b = parse_stmt();
        if (accept(TK_KW_ELSE)) s->c = parse_stmt();
        return s;
    }
    if (t->kind == TK_KW_WHILE) {
        advance();
        node_t *s = make_node(N_WHILE);
        if (!s) return NULL;
        if (expect(TK_LPAREN, "'('") != NX_OK) return NULL;
        s->a = parse_expr();
        if (expect(TK_RPAREN, "')'") != NX_OK) return NULL;
        s->b = parse_stmt();
        return s;
    }
    if (t->kind == TK_KW_FOR) {
        advance();
        node_t *s = make_node(N_FOR);
        if (!s) return NULL;
        if (expect(TK_LPAREN, "'('") != NX_OK) return NULL;
        /* init */
        if (is_type_kw(peek()->kind))      s->a = parse_var_decl();
        else if (!accept(TK_SEMI))         { s->a = parse_expr(); if (expect(TK_SEMI, "';'") != NX_OK) return NULL; }
        /* cond */
        if (!check(TK_SEMI))               s->b = parse_expr();
        if (expect(TK_SEMI, "';'") != NX_OK) return NULL;
        /* post */
        if (!check(TK_RPAREN))             s->c = parse_expr();
        if (expect(TK_RPAREN, "')'") != NX_OK) return NULL;
        s->d = parse_stmt();
        return s;
    }
    if (t->kind == TK_KW_RETURN) {
        advance();
        node_t *s = make_node(N_RETURN);
        if (!s) return NULL;
        if (!check(TK_SEMI)) s->a = parse_expr();
        if (expect(TK_SEMI, "';' after 'out'") != NX_OK) return NULL;
        return s;
    }
    if (t->kind == TK_KW_BREAK)    { advance(); if (expect(TK_SEMI, "';'") != NX_OK) return NULL; return make_node(N_BREAK); }
    if (t->kind == TK_KW_CONTINUE) { advance(); if (expect(TK_SEMI, "';'") != NX_OK) return NULL; return make_node(N_CONTINUE); }

    /* expression statement */
    node_t *e = parse_expr();
    if (!e) return NULL;
    if (expect(TK_SEMI, "';' after expression") != NX_OK) return NULL;
    node_t *s = make_node(N_EXPR_STMT);
    if (!s) return NULL;
    s->a = e;
    return s;
}

static node_t *parse_program(void) {
    node_t *root = make_node(N_BLOCK);
    if (!root) return NULL;
    node_t *tail = NULL;
    while (!check(TK_EOF)) {
        node_t *s = parse_stmt();
        if (!s) return NULL;
        if (!root->a) root->a = s;
        else          tail->next = s;
        tail = s;
    }
    return root;
}

/* ====================================================================== */
/*                              Value & VM                                 */
/* ====================================================================== */
typedef enum {
    V_NULL, V_INT, V_DEC, V_BOOL, V_STR,
} vtype_t;

typedef struct {
    vtype_t kind;
    int     ival;
    double  dval;
    char    sval[NX_MAX_STR];
} value_t;

static value_t v_null(void)        { value_t v = { V_NULL, 0, 0, "" }; return v; }
static value_t v_int (int i)       { value_t v = { V_INT,  i, 0, "" }; return v; }
static value_t v_dec (double d)    { value_t v = { V_DEC,  0, d, "" }; return v; }
static value_t v_bool(int b)       { value_t v = { V_BOOL, b ? 1 : 0, 0, "" }; return v; }
static value_t v_str (const char *s) {
    value_t v = { V_STR, 0, 0, {0} };
    strncpy(v.sval, s ? s : "", sizeof(v.sval) - 1);
    return v;
}

static int as_int  (value_t v) {
    switch (v.kind) {
        case V_INT:  return v.ival;
        case V_DEC:  return (int)v.dval;
        case V_BOOL: return v.ival ? 1 : 0;
        default:     return 0;
    }
}
static double as_dec(value_t v) {
    switch (v.kind) {
        case V_DEC:  return v.dval;
        case V_INT:  return (double)v.ival;
        case V_BOOL: return v.ival ? 1.0 : 0.0;
        default:     return 0.0;
    }
}
static int as_bool(value_t v) {
    switch (v.kind) {
        case V_INT:  return v.ival != 0;
        case V_DEC:  return v.dval != 0.0;
        case V_BOOL: return v.ival != 0;
        case V_STR:  return v.sval[0] != 0;
        default:     return 0;
    }
}

/* ---------- Variable + task tables (persistent across nxscript_eval) -- */
typedef struct {
    char    name[64];
    value_t val;
    bool    in_use;
} var_t;

typedef struct {
    char     name[64];
    node_t  *body;
    int      nparams;
    char     params[NX_MAX_ARGS][64];
    bool     in_use;
} task_t;

static var_t  g_vars [NX_MAX_VARS];
static task_t g_tasks[NX_MAX_TASKS];

static var_t *find_var(const char *name) {
    for (int i = 0; i < NX_MAX_VARS; i++)
        if (g_vars[i].in_use && strcmp(g_vars[i].name, name) == 0)
            return &g_vars[i];
    return NULL;
}

static var_t *new_var(const char *name, value_t v) {
    var_t *existing = find_var(name);
    if (existing) { existing->val = v; return existing; }
    for (int i = 0; i < NX_MAX_VARS; i++) {
        if (!g_vars[i].in_use) {
            strncpy(g_vars[i].name, name, sizeof(g_vars[i].name) - 1);
            g_vars[i].val = v;
            g_vars[i].in_use = true;
            return &g_vars[i];
        }
    }
    return NULL;
}

static task_t *find_task(const char *name) {
    for (int i = 0; i < NX_MAX_TASKS; i++)
        if (g_tasks[i].in_use && strcmp(g_tasks[i].name, name) == 0)
            return &g_tasks[i];
    return NULL;
}

static task_t *new_task(const char *name) {
    task_t *t = find_task(name);
    if (t) return t;
    for (int i = 0; i < NX_MAX_TASKS; i++) {
        if (!g_tasks[i].in_use) {
            strncpy(g_tasks[i].name, name, sizeof(g_tasks[i].name) - 1);
            g_tasks[i].in_use = true;
            g_tasks[i].nparams = 0;
            return &g_tasks[i];
        }
    }
    return NULL;
}

/* ---------- Script window output helper -------------------------------- */
/* Renders a string into the active script window, with word-wrap scroll. */
static void script_puts(const char *s) {
    if (!g_script_win || !g_script_win->in_use) {
        term_puts(s);
        return;
    }
    draw_target_t *t = &g_script_win->content;
    int x = 4;
    while (*s) {
        if (*s == '\n') {
            x = 4;
            g_script_text_y += FONT_GLYPH_H;
        } else {
            gfx_draw_char(t, x, g_script_text_y, *s,
                          g_script_text_color, g_script_win_color);
            x += FONT_GLYPH_W;
            if (x + FONT_GLYPH_W > (int)t->width) {
                x = 4;
                g_script_text_y += FONT_GLYPH_H;
            }
        }
        /* Scroll the content area up one line when we hit the bottom. */
        if (g_script_text_y + FONT_GLYPH_H > (int)t->height) {
            gfx_scroll_up(t, FONT_GLYPH_H, g_script_win_color);
            g_script_text_y -= FONT_GLYPH_H;
        }
        s++;
    }
    wm_mark_dirty();
}

static void script_putc(char c) {
    char buf[2] = { c, 0 };
    script_puts(buf);
}

/* ---------- Pretty-printing for builtin print() ------------------------ */
static void print_value(value_t v) {
    char buf[64];
    switch (v.kind) {
        case V_NULL: script_puts("none"); break;
        case V_INT:  itoa(v.ival, buf, 10); script_puts(buf); break;
        case V_BOOL: script_puts(v.ival ? "true" : "false"); break;
        case V_STR:  script_puts(v.sval); break;
        case V_DEC: {
            double d = v.dval;
            if (d < 0) { script_putc('-'); d = -d; }
            int whole = (int)d;
            itoa(whole, buf, 10);
            script_puts(buf);
            script_putc('.');
            double frac = d - (double)whole;
            for (int i = 0; i < 4; i++) frac *= 10.0;
            int fi = (int)frac;
            char fb[8];
            ksnprintf(fb, sizeof(fb), "%04d", fi);
            script_puts(fb);
            break;
        }
    }
}

/* ---------- Execution -------------------------------------------------- */
/* Control-flow sentinels propagated through return values. */
typedef enum { FX_NONE, FX_BREAK, FX_CONTINUE, FX_RETURN, FX_ERROR } flow_t;

static flow_t exec_node(node_t *n, value_t *out_val);

static flow_t exec_block(node_t *blk, value_t *out_val) {
    /* Carry the final statement's value out so top-level `out expr;`,
     * task-body returns, and bare-expression results all reach the caller
     * instead of being swallowed in a local. */
    *out_val = v_null();
    for (node_t *s = blk->a; s; s = s->next) {
        flow_t f = exec_node(s, out_val);
        if (f != FX_NONE) return f;
    }
    return FX_NONE;
}

static value_t bin_arith(node_kind_t op, value_t l, value_t r) {
    bool use_dec = (l.kind == V_DEC || r.kind == V_DEC);
    if (use_dec) {
        double a = as_dec(l), b = as_dec(r);
        switch (op) {
            case N_BIN_ADD: return v_dec(a + b);
            case N_BIN_SUB: return v_dec(a - b);
            case N_BIN_MUL: return v_dec(a * b);
            case N_BIN_DIV: return v_dec(b == 0.0 ? 0.0 : a / b);
            case N_BIN_MOD: return v_dec(0.0);     /* % not defined on float */
            default: break;
        }
    }
    int a = as_int(l), b = as_int(r);
    switch (op) {
        case N_BIN_ADD: return v_int(a + b);
        case N_BIN_SUB: return v_int(a - b);
        case N_BIN_MUL: return v_int(a * b);
        case N_BIN_DIV: return v_int(b == 0 ? 0 : a / b);
        case N_BIN_MOD: return v_int(b == 0 ? 0 : a % b);
        default: return v_null();
    }
}

static value_t bin_cmp(node_kind_t op, value_t l, value_t r) {
    /* Strings compare with strcmp when both sides are strings. */
    if (l.kind == V_STR && r.kind == V_STR) {
        int c = strcmp(l.sval, r.sval);
        switch (op) {
            case N_BIN_EQ: return v_bool(c == 0);
            case N_BIN_NE: return v_bool(c != 0);
            case N_BIN_LT: return v_bool(c <  0);
            case N_BIN_GT: return v_bool(c >  0);
            case N_BIN_LE: return v_bool(c <= 0);
            case N_BIN_GE: return v_bool(c >= 0);
            default:       return v_bool(0);
        }
    }
    if (l.kind == V_DEC || r.kind == V_DEC) {
        double a = as_dec(l), b = as_dec(r);
        switch (op) {
            case N_BIN_EQ: return v_bool(a == b);
            case N_BIN_NE: return v_bool(a != b);
            case N_BIN_LT: return v_bool(a <  b);
            case N_BIN_GT: return v_bool(a >  b);
            case N_BIN_LE: return v_bool(a <= b);
            case N_BIN_GE: return v_bool(a >= b);
            default:       return v_bool(0);
        }
    }
    int a = as_int(l), b = as_int(r);
    switch (op) {
        case N_BIN_EQ: return v_bool(a == b);
        case N_BIN_NE: return v_bool(a != b);
        case N_BIN_LT: return v_bool(a <  b);
        case N_BIN_GT: return v_bool(a >  b);
        case N_BIN_LE: return v_bool(a <= b);
        case N_BIN_GE: return v_bool(a >= b);
        default:       return v_bool(0);
    }
}

static flow_t exec_node(node_t *n, value_t *out_val) {
    if (!n) { *out_val = v_null(); return FX_NONE; }
    /* Cooperative Ctrl+C check.  Tree-walking interpreters can loop
     * forever on a malicious script; polling the keyboard's abort flag
     * at every node boundary keeps runaway tasks honest.  Cleared by
     * the shell after the FX_ERROR bubbles up. */
    if (keyboard_abort_requested()) {
        set_err(n->line, "execution aborted by Ctrl+C");
        return FX_ERROR;
    }
    switch (n->kind) {
        case N_INT:   *out_val = v_int (n->ival); return FX_NONE;
        case N_DEC:   *out_val = v_dec (n->dval); return FX_NONE;
        case N_STR:   *out_val = v_str (n->text); return FX_NONE;
        case N_BOOL:  *out_val = v_bool(n->ival); return FX_NONE;
        case N_NULL:  *out_val = v_null();        return FX_NONE;

        case N_IDENT: {
            var_t *v = find_var(n->text);
            if (!v) {
                /* Common mistake: writing `WindowColor` instead of
                 * `WindowColor(RED)`.  Identify it and tell the user
                 * exactly what they did wrong. */
                if (is_builtin(n->text)) {
                    set_err(n->line,
                            "'%s' is a function, did you mean %s(...)?",
                            n->text, n->text);
                } else if (find_task(n->text)) {
                    set_err(n->line,
                            "'%s' is a task, call it with %s(...)",
                            n->text, n->text);
                } else {
                    set_err(n->line, "unknown identifier '%s'", n->text);
                }
                return FX_ERROR;
            }
            *out_val = v->val;
            return FX_NONE;
        }

        case N_NEG: {
            value_t a; flow_t f = exec_node(n->a, &a); if (f) return f;
            if (a.kind == V_DEC) *out_val = v_dec(-as_dec(a));
            else                 *out_val = v_int(-as_int(a));
            return FX_NONE;
        }
        case N_NOT: {
            value_t a; flow_t f = exec_node(n->a, &a); if (f) return f;
            *out_val = v_bool(!as_bool(a));
            return FX_NONE;
        }

        case N_BIN_ADD: case N_BIN_SUB: case N_BIN_MUL:
        case N_BIN_DIV: case N_BIN_MOD: {
            value_t l, r;
            flow_t f1 = exec_node(n->a, &l); if (f1) return f1;
            flow_t f2 = exec_node(n->b, &r); if (f2) return f2;
            /* String + anything -> concatenation. */
            if (n->kind == N_BIN_ADD && (l.kind == V_STR || r.kind == V_STR)) {
                char buf[NX_MAX_STR];
                value_t a = l, b = r;
                char la[64], lb[64];
                if (a.kind == V_INT)  { itoa(a.ival, la, 10); a = v_str(la); }
                if (b.kind == V_INT)  { itoa(b.ival, lb, 10); b = v_str(lb); }
                if (a.kind == V_BOOL) a = v_str(a.ival ? "true" : "false");
                if (b.kind == V_BOOL) b = v_str(b.ival ? "true" : "false");
                if (a.kind != V_STR) a = v_str("");
                if (b.kind != V_STR) b = v_str("");
                ksnprintf(buf, sizeof(buf), "%s%s", a.sval, b.sval);
                *out_val = v_str(buf);
                return FX_NONE;
            }
            *out_val = bin_arith(n->kind, l, r);
            return FX_NONE;
        }
        case N_BIN_EQ: case N_BIN_NE:
        case N_BIN_LT: case N_BIN_GT:
        case N_BIN_LE: case N_BIN_GE: {
            value_t l, r;
            flow_t f1 = exec_node(n->a, &l); if (f1) return f1;
            flow_t f2 = exec_node(n->b, &r); if (f2) return f2;
            *out_val = bin_cmp(n->kind, l, r);
            return FX_NONE;
        }
        case N_AND: {
            value_t l, r;
            flow_t f1 = exec_node(n->a, &l); if (f1) return f1;
            if (!as_bool(l)) { *out_val = v_bool(0); return FX_NONE; }
            flow_t f2 = exec_node(n->b, &r); if (f2) return f2;
            *out_val = v_bool(as_bool(r));
            return FX_NONE;
        }
        case N_OR: {
            value_t l, r;
            flow_t f1 = exec_node(n->a, &l); if (f1) return f1;
            if (as_bool(l)) { *out_val = v_bool(1); return FX_NONE; }
            flow_t f2 = exec_node(n->b, &r); if (f2) return f2;
            *out_val = v_bool(as_bool(r));
            return FX_NONE;
        }

        case N_VARDECL: {
            value_t init = v_null();
            if (n->a) {
                flow_t f = exec_node(n->a, &init);
                if (f) return f;
            } else {
                /* default per type */
                switch ((tok_kind_t)n->ival) {
                    case TK_KW_INT:  init = v_int(0);    break;
                    case TK_KW_DEC:  init = v_dec(0.0);  break;
                    case TK_KW_BOOL: init = v_bool(0);   break;
                    case TK_KW_STR:  init = v_str("");   break;
                    default:         break;
                }
            }
            new_var(n->text, init);
            *out_val = init;
            return FX_NONE;
        }
        case N_ASSIGN: {
            /* Refuse to clobber a builtin's name with a variable - the
             * resulting "WindowColor = 7;" silently broke calls to
             * WindowColor(...) later in the same script.  Surface it. */
            if (is_builtin(n->text)) {
                set_err(n->line,
                        "cannot assign to builtin function '%s' - "
                        "did you mean %s(...)?",
                        n->text, n->text);
                return FX_ERROR;
            }
            value_t v;
            flow_t f = exec_node(n->a, &v); if (f) return f;
            var_t *vp = find_var(n->text);
            if (!vp) {
                /* implicit decl on first assignment */
                new_var(n->text, v);
            } else {
                vp->val = v;
            }
            *out_val = v;
            return FX_NONE;
        }

        case N_CALL: {
            /* Validate builtin arity FIRST so a typo like
             * "WindowColor(RED, BLUE)" is caught with a real message
             * instead of silently being treated as an unknown task. */
            const builtin_t *bi = find_builtin(n->text);
            if (bi && (n->nargs < bi->min_args || n->nargs > bi->max_args)) {
                set_err(n->line,
                        "wrong argument count for '%s' (expected %d, got %d)",
                        n->text, bi->min_args, n->nargs);
                return FX_ERROR;
            }

            /* Builtin: print */
            if (strcmp(n->text, "print") == 0) {
                for (int i = 0; i < n->nargs; i++) {
                    value_t a;
                    flow_t f = exec_node(n->args[i], &a); if (f) return f;
                    print_value(a);
                    if (i + 1 < n->nargs) script_putc(' ');
                }
                script_putc('\n');
                *out_val = v_null();
                return FX_NONE;
            }
            /* Builtin: CreateWindow(x, y, w, h, title) */
            if (strcmp(n->text, "CreateWindow") == 0) {
                value_t ax, ay, aw, ah, atitle;
                { flow_t f = exec_node(n->args[0], &ax); if (f) return f; }
                { flow_t f = exec_node(n->args[1], &ay); if (f) return f; }
                { flow_t f = exec_node(n->args[2], &aw); if (f) return f; }
                { flow_t f = exec_node(n->args[3], &ah); if (f) return f; }
                { flow_t f = exec_node(n->args[4], &atitle); if (f) return f; }
                const char *title = (atitle.kind == V_STR) ? atitle.sval : "Script";
                window_t *win = wm_create_window(as_int(ax), as_int(ay),
                                                  as_int(aw), as_int(ah), title);
                if (!win) {
                    set_err(n->line, "CreateWindow: out of window slots");
                    return FX_ERROR;
                }
                g_script_win    = win;
                g_script_text_y = 4;
                gfx_clear(&win->content, g_script_win_color);
                wm_mark_dirty();
                wm_present();                /* paint immediately */
                *out_val = v_int(win->id);
                return FX_NONE;
            }
            /* Builtin: WindowColor(argb) - fill script window bg
             *
             * Updates the saved colour regardless of whether a script
             * window already exists, AND forces an immediate compose so
             * the user sees the redraw without waiting for the next
             * wm_tick().  The previous code only marked dirty, which
             * left the new colour invisible if the next event was a
             * blocking keyboard wait. */
            if (strcmp(n->text, "WindowColor") == 0) {
                value_t col;
                { flow_t f = exec_node(n->args[0], &col); if (f) return f; }
                g_script_win_color = (uint32_t)as_int(col);
                if (g_script_win && g_script_win->in_use) {
                    gfx_clear(&g_script_win->content, g_script_win_color);
                    g_script_text_y = 4;
                    wm_mark_dirty();
                    wm_present();
                }
                *out_val = v_null();
                return FX_NONE;
            }
            /* Builtin: TextColor(argb) - set print() fg colour */
            if (strcmp(n->text, "TextColor") == 0) {
                value_t col;
                { flow_t f = exec_node(n->args[0], &col); if (f) return f; }
                g_script_text_color = (uint32_t)as_int(col);
                *out_val = v_null();
                return FX_NONE;
            }
            /* User-defined task */
            task_t *t = find_task(n->text);
            if (!t) {
                /* Better diagnostic: distinguish "unknown name" from
                 * "name exists but isn't callable" (i.e. a variable). */
                if (find_var(n->text)) {
                    set_err(n->line,
                            "'%s' is a variable, not a function", n->text);
                } else {
                    set_err(n->line, "unknown function '%s'", n->text);
                }
                return FX_ERROR;
            }
            /* Save current variable bindings for the param names so we can
             * restore them on return - very simple "shadowing" model. */
            value_t saved[NX_MAX_ARGS];
            bool    had   [NX_MAX_ARGS];
            int     nargs = t->nparams < n->nargs ? t->nparams : n->nargs;
            for (int i = 0; i < nargs; i++) {
                var_t *vp = find_var(t->params[i]);
                had[i]   = vp != NULL;
                saved[i] = vp ? vp->val : v_null();
                value_t a;
                flow_t f = exec_node(n->args[i], &a); if (f) return f;
                new_var(t->params[i], a);
            }
            value_t rv = v_null();
            flow_t f = exec_node(t->body, &rv);
            if (f == FX_RETURN) f = FX_NONE;
            /* Restore. */
            for (int i = 0; i < nargs; i++) {
                if (had[i]) {
                    var_t *vp = find_var(t->params[i]);
                    if (vp) vp->val = saved[i];
                } else {
                    var_t *vp = find_var(t->params[i]);
                    if (vp) vp->in_use = false;
                }
            }
            *out_val = rv;
            return f;
        }

        case N_TASK: {
            task_t *t = new_task(n->text);
            if (!t) { set_err(0, "too many tasks"); return FX_ERROR; }
            t->body    = n->a;
            t->nparams = n->nargs;
            for (int i = 0; i < n->nargs && i < NX_MAX_ARGS; i++) {
                strncpy(t->params[i], n->args[i]->text, sizeof(t->params[i]) - 1);
            }
            *out_val = v_null();
            return FX_NONE;
        }

        case N_BLOCK: return exec_block(n, out_val);

        case N_IF: {
            value_t cond;
            flow_t f = exec_node(n->a, &cond); if (f) return f;
            if (as_bool(cond))    return exec_node(n->b, out_val);
            else if (n->c)        return exec_node(n->c, out_val);
            return FX_NONE;
        }
        case N_WHILE: {
            while (1) {
                value_t cond;
                flow_t f = exec_node(n->a, &cond); if (f) return f;
                if (!as_bool(cond)) break;
                f = exec_node(n->b, out_val);
                if (f == FX_BREAK) break;
                if (f == FX_RETURN || f == FX_ERROR) return f;
            }
            return FX_NONE;
        }
        case N_FOR: {
            value_t tmp;
            if (n->a) { flow_t f = exec_node(n->a, &tmp); if (f) return f; }
            while (1) {
                if (n->b) {
                    value_t cond;
                    flow_t f = exec_node(n->b, &cond); if (f) return f;
                    if (!as_bool(cond)) break;
                }
                flow_t f = exec_node(n->d, &tmp);
                if (f == FX_BREAK) break;
                if (f == FX_RETURN || f == FX_ERROR) return f;
                if (n->c) { f = exec_node(n->c, &tmp); if (f) return f; }
            }
            return FX_NONE;
        }
        case N_RETURN: {
            value_t v = v_null();
            if (n->a) {
                flow_t f = exec_node(n->a, &v); if (f) return f;
            }
            *out_val = v;
            return FX_RETURN;
        }
        case N_BREAK:    return FX_BREAK;
        case N_CONTINUE: return FX_CONTINUE;
        case N_EXPR_STMT: {
            value_t v;
            flow_t f = exec_node(n->a, &v); if (f) return f;
            *out_val = v;
            return FX_NONE;
        }
    }
    set_err(0, "unhandled node kind %d", n->kind);
    return FX_ERROR;
}

/* ====================================================================== */
/*                              Public API                                 */
/* ====================================================================== */
void nxscript_init(void) {
    nxscript_reset();
}

void nxscript_reset(void) {
    memset(g_vars,  0, sizeof(g_vars));
    memset(g_tasks, 0, sizeof(g_tasks));
    g_err[0] = 0;

    /* Reset script window context (but keep the window open if it exists). */
    g_script_win        = NULL;
    g_script_win_color  = 0xFF000020;
    g_script_text_color = 0xFFFFFFFF;
    g_script_text_y     = 4;

    /* Pre-load ARGB colour constants so scripts can write e.g.
     *   WindowColor(BLUE);  TextColor(YELLOW);              */
    new_var("RED",    v_int((int)0xFFFF4040u));
    new_var("GREEN",  v_int((int)0xFF40FF80u));
    new_var("BLUE",   v_int((int)0xFF4080FFu));
    new_var("YELLOW", v_int((int)0xFFFFE040u));
    new_var("CYAN",   v_int((int)0xFF40E0E0u));
    new_var("WHITE",  v_int((int)0xFFFFFFFFu));
    new_var("BLACK",  v_int((int)0xFF000000u));
    new_var("NAVY",   v_int((int)0xFF000020u));
}

const char *nxscript_last_error(void) { return g_err; }

int nxscript_eval(const char *source) {
    if (!source) return NX_OK;
    g_src = source;
    g_pos = 0;
    g_err[0] = 0;
    g_node_used = 0;
    g_p = 0;
    int r = lex(); if (r != NX_OK) return r;
    node_t *prog = parse_program();
    if (!prog) return NX_ERR_PARSE;

    value_t out;
    flow_t f = exec_node(prog, &out);
    if (f == FX_ERROR) return NX_ERR_RUNTIME;

    /* If the user typed a bare top-level `out expr;`, surface the value. */
    if (f == FX_RETURN && out.kind != V_NULL) {
        term_puts("=> ");
        print_value(out);
        term_putc('\n');
    }
    return NX_OK;
}

bool nxscript_looks_like(const char *line) {
    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;
    /* Match against the set of unmistakable NXScript leading tokens. */
    static const char *prefixes[] = {
        "task ", "num ", "txt ", "numdec ", "bit ",
        "if ", "if(", "loop ", "loop(", "step ", "step(",
        "pick ", "pick(", "out ", "out;", "print(",
        "CreateWindow(", "WindowColor(", "TextColor(",
        NULL,
    };
    for (int i = 0; prefixes[i]; i++) {
        size_t pl = strlen(prefixes[i]);
        if (strncmp(line, prefixes[i], pl) == 0) return true;
    }
    return false;
}
