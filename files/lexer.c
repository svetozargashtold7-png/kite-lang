#include "kite.h"

static void tok_push(Lexer *l, TKind kind, const char *str, double num, int line) {
    if (l->buf_len >= l->buf_cap) {
        l->buf_cap = l->buf_cap ? l->buf_cap * 2 : 256;
        l->buf = realloc(l->buf, (size_t)l->buf_cap * sizeof(Token));
    }
    Token *t  = &l->buf[l->buf_len++];
    t->kind   = kind;
    t->str    = str ? strdup(str) : NULL;
    t->num    = num;
    t->line   = line;
}

Lexer *lexer_new(const char *src) {
    Lexer *l = calloc(1, sizeof(Lexer));
    l->src   = src;
    l->line  = 1;
    return l;
}

void lexer_free(Lexer *l) {
    for (int i = 0; i < l->buf_len; i++) free(l->buf[i].str);
    free(l->buf);
    free(l);
}

static struct { const char *word; TKind kind; } KEYWORDS[] = {
    {"set",    TK_SET},    {"def",    TK_DEF},
    {"give",   TK_GIVE},   {"end",    TK_END},
    {"when",   TK_WHEN},   {"orwhen", TK_ORWHEN},
    {"else",   TK_ELSE},   {"loop",   TK_LOOP},
    {"while",  TK_WHILE},  {"for",    TK_FOR},
    {"in",     TK_IN},     {"true",   TK_TRUE},
    {"false",  TK_FALSE},  {"nil",    TK_NIL},
    {"and",    TK_AND},    {"or",     TK_OR},
    {"not",    TK_NOT},    {"break",  TK_BREAK},
    {"next",   TK_NEXT},    {"use",    TK_USE},
    {"do",     TK_DO},
    {"err",    TK_ERR},
    {"obj",     TK_OBJ},
    {"extends", TK_EXTENDS},
    {"is",      TK_IS},
    {"super",   TK_SUPER},
    {NULL, 0}
};

/* Append one char to a dynamic buffer */
static void dbuf_push(char **buf, int *len, int *cap, char c) {
    if (*len + 1 >= *cap) {
        *cap = *cap ? *cap * 2 : 256;
        *buf = realloc(*buf, (size_t)*cap);
    }
    (*buf)[(*len)++] = c;
}

void lexer_tokenize(Lexer *l) {
    const char *s   = l->src;
    int i           = 0;
    int line        = 1;
    int prev_nl     = 1;

    while (s[i]) {
        /* whitespace */
        while (s[i] == ' ' || s[i] == '\t' || s[i] == '\r') i++;

        /* comment */
        if (s[i] == '#') {
            while (s[i] && s[i] != '\n') i++;
            continue;
        }

        /* newline */
        if (s[i] == '\n') {
            if (!prev_nl) tok_push(l, TK_NEWLINE, NULL, 0, line);
            line++; i++; prev_nl = 1;
            continue;
        }
        prev_nl = 0;

        /* string literal — supports ${expr} interpolation */
        if (s[i] == '"' || s[i] == '\'') {
            char q = s[i++];

            /* check if this string contains ${ */
            int has_interp = 0;
            for (int k = i; s[k] && s[k] != q; k++) {
                if (s[k] == '\\') { k++; continue; }
                if (s[k] == '$' && s[k+1] == '{') { has_interp = 1; break; }
            }

            /* build token content into dynamic buffer */
            char *buf = NULL; int blen = 0, bcap = 0;

            while (s[i] && s[i] != q) {
                if (s[i] == '\\') {
                    i++;
                    char esc;
                    switch (s[i]) {
                        case 'n':  esc = '\n'; break;
                        case 't':  esc = '\t'; break;
                        case '\\': esc = '\\'; break;
                        case '"':  esc = '"';  break;
                        case '\'': esc = '\''; break;
                        case '$':  esc = '$';  break;
                        default:   esc = s[i]; break;
                    }
                    dbuf_push(&buf, &blen, &bcap, esc);
                    i++;
                } else if (has_interp && s[i] == '$' && s[i+1] == '{') {
                    /* \x01 marks start, \x02 marks end of embedded expr */
                    dbuf_push(&buf, &blen, &bcap, '\x01');
                    i += 2; /* skip ${ */
                    int depth = 1;
                    while (s[i] && depth > 0) {
                        if      (s[i] == '{') depth++;
                        else if (s[i] == '}') {
                            depth--;
                            if (depth == 0) { i++; break; }
                        }
                        dbuf_push(&buf, &blen, &bcap, s[i++]);
                    }
                    dbuf_push(&buf, &blen, &bcap, '\x02');
                } else {
                    dbuf_push(&buf, &blen, &bcap, s[i++]);
                }
            }
            if (s[i] == q) i++;

            /* null-terminate */
            dbuf_push(&buf, &blen, &bcap, '\0');

            tok_push(l, has_interp ? TK_FMTSTR : TK_STR, buf, 0, line);
            free(buf);
            continue;
        }

        /* number */
        if (isdigit((unsigned char)s[i]) ||
            (s[i] == '-' && isdigit((unsigned char)s[i+1]) && prev_nl)) {
            char nbuf[64]; int ni = 0;
            if (s[i] == '-') nbuf[ni++] = s[i++];
            while (isdigit((unsigned char)s[i]) || s[i] == '.') nbuf[ni++] = s[i++];
            nbuf[ni] = '\0';
            tok_push(l, TK_NUM, NULL, atof(nbuf), line);
            continue;
        }

        /* identifier / keyword */
        if (isalpha((unsigned char)s[i]) || s[i] == '_') {
            char wbuf[256]; int wi = 0;
            while (isalnum((unsigned char)s[i]) || s[i] == '_') wbuf[wi++] = s[i++];
            wbuf[wi] = '\0';
            TKind kk = TK_IDENT;
            for (int k = 0; KEYWORDS[k].word; k++)
                if (strcmp(wbuf, KEYWORDS[k].word) == 0) { kk = KEYWORDS[k].kind; break; }
            tok_push(l, kk, kk == TK_IDENT ? wbuf : NULL, 0, line);
            continue;
        }

        /* two-char operators */
        struct { const char *s; TKind k; } ops2[] = {
            {"->", TK_ARROW},  {"..", TK_DOTDOT},
            {"==", TK_EQ},     {"!=", TK_NEQ},
            {"<=", TK_LE},     {">=", TK_GE},
            {"+=", TK_PLUSEQ}, {"-=", TK_MINUSEQ},
            {"*=", TK_STAREQ}, {"/=", TK_SLASHEQ},
            {NULL, 0}
        };
        int matched = 0;
        for (int k = 0; ops2[k].s; k++) {
            if (s[i] == ops2[k].s[0] && s[i+1] == ops2[k].s[1]) {
                tok_push(l, ops2[k].k, NULL, 0, line);
                i += 2; matched = 1; break;
            }
        }
        if (matched) continue;

        /* single-char */
        struct { char c; TKind k; } ops1[] = {
            {'+', TK_PLUS},  {'-', TK_MINUS}, {'*', TK_STAR},
            {'/', TK_SLASH}, {'%', TK_PERCENT},{'<', TK_LT},
            {'>', TK_GT},    {'=', TK_ASSIGN}, {'^', TK_CARET},
            {'(', TK_LPAREN},{')', TK_RPAREN}, {'[', TK_LBRACKET},
            {']', TK_RBRACKET},{'{', TK_LBRACE},{'}', TK_RBRACE},
            {',', TK_COMMA}, {':', TK_COLON},  {'.', TK_DOT},
            {0, 0}
        };
        int found = 0;
        for (int k = 0; ops1[k].c; k++) {
            if (s[i] == ops1[k].c) {
                tok_push(l, ops1[k].k, NULL, 0, line);
                i++; found = 1; break;
            }
        }
        if (!found) {
            fprintf(stderr, "kite: unknown char '%c' (0x%02x) at line %d\n",
                    s[i], (unsigned char)s[i], line);
            i++;
        }
    }
    tok_push(l, TK_EOF, NULL, 0, line);
}
