#include "kite.h"

/* ══════════════════════════════════════════════════════════
   PARSER STATE
══════════════════════════════════════════════════════════ */
typedef struct {
    Lexer *l;
    int    pos;
    char   errbuf[512];
    int    had_error;
} Parser;

static Token *peek(Parser *p)  { return &p->l->buf[p->pos]; }
static Token *peek2(Parser *p) {
    int nx = p->pos+1 < p->l->buf_len ? p->pos+1 : p->l->buf_len-1;
    return &p->l->buf[nx];
}
static Token *advance(Parser *p) {
    Token *t = &p->l->buf[p->pos];
    if (p->pos < p->l->buf_len-1) p->pos++;
    return t;
}
static int check(Parser *p, TKind k)  { return peek(p)->kind  == k; }
static int check2(Parser *p, TKind k) { return peek2(p)->kind == k; }

static Token *eat(Parser *p, TKind k, const char *ctx) {
    if (!check(p,k)) {
        snprintf(p->errbuf, sizeof(p->errbuf),
            "Expected token %d, got %d ('%s') at line %d [%s]",
            k, peek(p)->kind, peek(p)->str ? peek(p)->str : "?",
            peek(p)->line, ctx);
        p->had_error = 1;
    }
    return advance(p);
}

static void skip_nl(Parser *p) { while (check(p,TK_NEWLINE)) advance(p); }

/* ── Node helpers ───────────────────────────────────────── */
static Node *mknode(NKind k, int line) {
    Node *n = calloc(1, sizeof(Node));
    n->kind  = k; n->line = line;
    return n;
}

static void nl_push(NodeList *nl, Node *n) {
    if (nl->len >= nl->cap) {
        nl->cap = nl->cap ? nl->cap*2 : 4;
        nl->items = realloc(nl->items, (size_t)nl->cap * sizeof(Node*));
    }
    nl->items[nl->len++] = n;
}

void node_free(Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_STR:
        case ND_IDENT:
        case ND_USE: free(n->str); break;
        case ND_OBJ:
            free(n->obj_def.name);
            free(n->obj_def.parent);
            for (int i=0;i<n->obj_def.body.len;i++) node_free(n->obj_def.body.items[i]);
            free(n->obj_def.body.items);
            break;
        case ND_DO:
            node_free(n->do_catch.body);
            for (int i=0;i<n->do_catch.nhandlers;i++) {
                free(n->do_catch.handlers[i].err_type);
                node_free(n->do_catch.handlers[i].body);
            }
            free(n->do_catch.handlers);
            break;
        case ND_FMTSTR:
            for (int i=0;i<n->fmtstr.len;i++) node_free(n->fmtstr.items[i]);
            free(n->fmtstr.items); break;
        case ND_SET: free(n->set.name); node_free(n->set.val); break;
        case ND_ASSIGN:
            free(n->assign.op); node_free(n->assign.target); node_free(n->assign.val); break;
        case ND_BINOP: free(n->binop.op); node_free(n->binop.left); node_free(n->binop.right); break;
        case ND_UNOP:  free(n->unop.op);  node_free(n->unop.expr); break;
        case ND_CALL:
            node_free(n->call.callee);
            for (int i=0;i<n->call.args.len;i++) node_free(n->call.args.items[i]);
            free(n->call.args.items); break;
        case ND_INDEX: node_free(n->idx.obj); node_free(n->idx.idx); break;
        case ND_PROP:  node_free(n->prop.obj); free(n->prop.prop); break;
        case ND_LIST:
            for (int i=0;i<n->list.len;i++) node_free(n->list.items[i]);
            free(n->list.items); break;
        case ND_MAP:
            for (int i=0;i<n->map.len;i++) { free(n->map.pairs[i].key); node_free(n->map.pairs[i].val); }
            free(n->map.pairs); break;
        case ND_LAMBDA:
            for (int i=0;i<n->lambda.nparams;i++) free(n->lambda.params[i]);
            free(n->lambda.params); node_free(n->lambda.body); break;
        case ND_DEF:
            free(n->def.name);
            for (int i=0;i<n->def.nparams;i++) free(n->def.params[i]);
            free(n->def.params); node_free(n->def.body); break;
        case ND_WHEN:
            for (int i=0;i<n->when.nbranch;i++) {
                node_free(n->when.conds[i]); node_free(n->when.bodies[i]);
            }
            free(n->when.conds); free(n->when.bodies); break;
        case ND_LOOP_WHILE: node_free(n->loop_while.cond); node_free(n->loop_while.body); break;
        case ND_LOOP_FOR:
            free(n->loop_for.var); node_free(n->loop_for.iter); node_free(n->loop_for.body); break;
        case ND_GIVE:
        case ND_EXPR_STMT: node_free(n->child); break;
        case ND_BLOCK:
        case ND_PROGRAM:
            for (int i=0;i<n->block.len;i++) node_free(n->block.items[i]);
            free(n->block.items); break;
        default: break;
    }
    free(n);
}

/* ── Forward decls ──────────────────────────────────────── */
static Node *parse_stmt(Parser *p);
static Node *parse_expr(Parser *p);
static Node *parse_body(Parser *p);
static Node *try_lambda(Parser *p);

/* ══════════════════════════════════════════════════════════
   STATEMENT PARSERS
══════════════════════════════════════════════════════════ */
static char **parse_params(Parser *p, int *np) {
    eat(p, TK_LPAREN, "params");
    char **params = NULL; *np = 0; int cap = 0;
    while (!check(p,TK_RPAREN) && !check(p,TK_EOF)) {
        Token *t = eat(p, TK_IDENT, "param");
        if (*np >= cap) { cap = cap ? cap*2 : 4; params = realloc(params,(size_t)cap*sizeof(char*)); }
        params[(*np)++] = strdup(t->str);
        if (check(p,TK_COMMA)) advance(p);
    }
    eat(p, TK_RPAREN, "params)");
    return params;
}

/* Parse block body after ':' until end/orwhen/else (for when branches) */
static Node *parse_branch_body(Parser *p) {
    eat(p, TK_COLON, "branch:");
    skip_nl(p);
    Node *b = mknode(ND_BLOCK, peek(p)->line);
    while (!check(p,TK_END) && !check(p,TK_ORWHEN) && !check(p,TK_ELSE) && !check(p,TK_EOF)) {
        if (check(p,TK_NEWLINE)) { advance(p); continue; }
        nl_push(&b->block, parse_stmt(p));
        skip_nl(p);
    }
    return b;
}

/* Parse block body after ':' until 'end' (for def/loop) */
static Node *parse_body(Parser *p) {
    eat(p, TK_COLON, "body:");
    skip_nl(p);
    Node *b = mknode(ND_BLOCK, peek(p)->line);
    while (!check(p,TK_END) && !check(p,TK_EOF)) {
        if (check(p,TK_NEWLINE)) { advance(p); continue; }
        nl_push(&b->block, parse_stmt(p));
        skip_nl(p);
    }
    eat(p, TK_END, "end");
    return b;
}

static Node *parse_def(Parser *p) {
    int line = peek(p)->line; advance(p);
    Token *name = eat(p, TK_IDENT, "def name");
    int np = 0; char **params = parse_params(p, &np);
    Node *body = parse_body(p);
    Node *n = mknode(ND_DEF, line);
    n->def.name = strdup(name->str); n->def.params = params; n->def.nparams = np;
    n->def.body = body;
    return n;
}

static Node *parse_when(Parser *p) {
    int line = peek(p)->line; advance(p);
    Node **conds = NULL, **bodies = NULL; int nb = 0, cap = 0;

    #define PUSH_BRANCH(c,b) do { \
        if (nb >= cap) { cap = cap ? cap*2 : 4; \
            conds  = realloc(conds,  (size_t)cap*sizeof(Node*)); \
            bodies = realloc(bodies, (size_t)cap*sizeof(Node*)); } \
        conds[nb]=(c); bodies[nb]=(b); nb++; } while(0)

    PUSH_BRANCH(parse_expr(p), parse_branch_body(p));
    skip_nl(p);

    while (check(p,TK_ORWHEN)) {
        advance(p);
        PUSH_BRANCH(parse_expr(p), parse_branch_body(p));
        skip_nl(p);
    }
    if (check(p,TK_ELSE)) {
        advance(p);
        eat(p, TK_COLON, "else:");
        skip_nl(p);
        Node *eb = mknode(ND_BLOCK, peek(p)->line);
        while (!check(p,TK_END) && !check(p,TK_EOF)) {
            if (check(p,TK_NEWLINE)) { advance(p); continue; }
            nl_push(&eb->block, parse_stmt(p));
            skip_nl(p);
        }
        PUSH_BRANCH(NULL, eb);
    }
    eat(p, TK_END, "when end");
    Node *n = mknode(ND_WHEN, line);
    n->when.conds = conds; n->when.bodies = bodies; n->when.nbranch = nb;
    return n;
    #undef PUSH_BRANCH
}

static Node *parse_loop(Parser *p) {
    int line = peek(p)->line; advance(p);
    if (check(p,TK_WHILE)) {
        advance(p);
        Node *cond = parse_expr(p);
        Node *body = parse_body(p);
        Node *n = mknode(ND_LOOP_WHILE, line);
        n->loop_while.cond = cond; n->loop_while.body = body;
        return n;
    }
    if (check(p,TK_FOR)) {
        advance(p);
        Token *var = eat(p, TK_IDENT, "for var");
        eat(p, TK_IN, "in");
        Node *iter = parse_expr(p);
        Node *body = parse_body(p);
        Node *n = mknode(ND_LOOP_FOR, line);
        n->loop_for.var = strdup(var->str); n->loop_for.iter = iter; n->loop_for.body = body;
        return n;
    }
    p->had_error = 1;
    snprintf(p->errbuf, sizeof(p->errbuf), "Expected 'while' or 'for' after 'loop' at line %d", line);
    return mknode(ND_NIL, line);
}

static Node *parse_stmt(Parser *p) {
    skip_nl(p);
    int line = peek(p)->line;
    switch (peek(p)->kind) {
        case TK_SET: {
            advance(p);
            Token *name = eat(p, TK_IDENT, "set name");
            eat(p, TK_ASSIGN, "=");
            Node *val = parse_expr(p);
            Node *n = mknode(ND_SET, line);
            n->set.name = strdup(name->str); n->set.val = val;
            return n;
        }
        case TK_DEF:  return parse_def(p);
        case TK_WHEN: return parse_when(p);
        case TK_LOOP: return parse_loop(p);
        case TK_GIVE: {
            advance(p);
            Node *n = mknode(ND_GIVE, line);
            n->child = (check(p,TK_NEWLINE)||check(p,TK_EOF)) ? mknode(ND_NIL,line) : parse_expr(p);
            return n;
        }
        case TK_BREAK: advance(p); return mknode(ND_BREAK, line);
        case TK_NEXT:  advance(p); return mknode(ND_NEXT,  line);
        case TK_USE: {
            advance(p);
            Token *nm = eat(p, TK_IDENT, "use name");
            Node *n = mknode(ND_USE, line);
            n->str = strdup(nm->str);
            return n;
        }
        case TK_OBJ: {
            int ln = peek(p)->line; advance(p); /* eat 'obj' */
            Token *name = eat(p, TK_IDENT, "obj name");
            char *parent = NULL;
            if (check(p, TK_EXTENDS)) {
                advance(p);
                Token *pname = eat(p, TK_IDENT, "parent name");
                parent = strdup(pname->str);
            }
            eat(p, TK_COLON, "obj:");
            skip_nl(p);
            Node *n = mknode(ND_OBJ, ln);
            n->obj_def.name   = strdup(name->str);
            n->obj_def.parent = parent;
            while (!check(p,TK_END) && !check(p,TK_EOF)) {
                if (check(p,TK_NEWLINE)) { advance(p); continue; }
                nl_push(&n->obj_def.body, parse_stmt(p));
                skip_nl(p);
            }
            eat(p, TK_END, "obj end");
            return n;
        }
        case TK_DO: {
            advance(p); /* eat 'do' */
            eat(p, TK_COLON, "do:");
            skip_nl(p);
            /* parse body until first 'err' or 'end' */
            Node *body = mknode(ND_BLOCK, peek(p)->line);
            while (!check(p,TK_ERR) && !check(p,TK_END) && !check(p,TK_EOF)) {
                if (check(p,TK_NEWLINE)) { advance(p); continue; }
                nl_push(&body->block, parse_stmt(p));
                skip_nl(p);
            }
            /* parse err handlers */
            ErrHandler *handlers = NULL; int nh = 0, hcap = 0;
            while (check(p, TK_ERR)) {
                advance(p); /* eat 'err' */
                /* optional error type name */
                char *etype = NULL;
                if (check(p, TK_IDENT)) etype = strdup(advance(p)->str);
                eat(p, TK_COLON, "err:");
                skip_nl(p);
                Node *hbody = mknode(ND_BLOCK, peek(p)->line);
                while (!check(p,TK_ERR) && !check(p,TK_END) && !check(p,TK_EOF)) {
                    if (check(p,TK_NEWLINE)) { advance(p); continue; }
                    nl_push(&hbody->block, parse_stmt(p));
                    skip_nl(p);
                }
                if (nh >= hcap) {
                    hcap = hcap ? hcap*2 : 4;
                    handlers = realloc(handlers, (size_t)hcap * sizeof(ErrHandler));
                }
                handlers[nh].err_type = etype;
                handlers[nh].body     = hbody;
                nh++;
            }
            eat(p, TK_END, "do end");
            Node *n = mknode(ND_DO, line);
            n->do_catch.body      = body;
            n->do_catch.handlers  = handlers;
            n->do_catch.nhandlers = nh;
            return n;
        }
        default: {
            Node *e = parse_expr(p);
            Node *s = mknode(ND_EXPR_STMT, line);
            s->child = e; return s;
        }
    }
}

/* ══════════════════════════════════════════════════════════
   EXPRESSION PARSERS  (Pratt precedence)
══════════════════════════════════════════════════════════ */
static Node *parse_primary(Parser *p);
static Node *parse_postfix(Parser *p);
static Node *parse_unary(Parser *p);
static Node *parse_power(Parser *p);
static Node *parse_mul(Parser *p);
static Node *parse_add(Parser *p);
static Node *parse_cmp(Parser *p);
static Node *parse_and(Parser *p);
static Node *parse_or(Parser *p);
static Node *parse_assign(Parser *p);

static Node *parse_expr(Parser *p) { return parse_assign(p); }

/* Lambda: x -> expr  |  (a,b) -> expr */
static Node *try_lambda(Parser *p) {
    int line = peek(p)->line;

    /* single-param: IDENT -> */
    if (check(p,TK_IDENT) && check2(p,TK_ARROW)) {
        Token *pname = advance(p); advance(p); /* eat name and -> */
        char **params = malloc(sizeof(char*));
        params[0] = strdup(pname->str);
        Node *body;
        if (check(p,TK_COLON)) {
            body = parse_body(p);
        } else {
            Node *e = parse_expr(p);
            Node *b = mknode(ND_BLOCK, line);
            Node *g = mknode(ND_GIVE,  line); g->child = e;
            nl_push(&b->block, g); body = b;
        }
        Node *n = mknode(ND_LAMBDA, line);
        n->lambda.params = params; n->lambda.nparams = 1; n->lambda.body = body;
        return n;
    }

    /* multi-param: ( a, b, ... ) -> */
    if (check(p,TK_LPAREN)) {
        int save = p->pos;
        advance(p);
        int np = 0, cap = 0; char **params = NULL; int ok = 1;
        while (!check(p,TK_RPAREN) && !check(p,TK_EOF)) {
            if (!check(p,TK_IDENT)) { ok = 0; break; }
            Token *t = advance(p);
            if (np >= cap) { cap = cap?cap*2:4; params=realloc(params,(size_t)cap*sizeof(char*)); }
            params[np++] = strdup(t->str);
            if (check(p,TK_COMMA)) advance(p);
        }
        if (!ok || !check(p,TK_RPAREN)) {
            for (int i=0;i<np;i++) { free(params[i]); } free(params);
            p->pos = save; return NULL;
        }
        advance(p); /* ) */
        if (!check(p,TK_ARROW)) {
            for (int i=0;i<np;i++) { free(params[i]); } free(params);
            p->pos = save; return NULL;
        }
        advance(p); /* -> */
        Node *body;
        if (check(p,TK_COLON)) {
            body = parse_body(p);
        } else {
            Node *e = parse_expr(p);
            Node *b = mknode(ND_BLOCK, line);
            Node *g = mknode(ND_GIVE,  line); g->child = e;
            nl_push(&b->block, g); body = b;
        }
        Node *n = mknode(ND_LAMBDA, line);
        n->lambda.params = params; n->lambda.nparams = np; n->lambda.body = body;
        return n;
    }
    return NULL;
}

static Node *parse_primary(Parser *p) {
    int line = peek(p)->line;

    /* try lambda first */
    if ((check(p,TK_IDENT) && check2(p,TK_ARROW)) || check(p,TK_LPAREN)) {
        int save = p->pos;
        Node *lam = try_lambda(p);
        if (lam) return lam;
        p->pos = save;
    }

    switch (peek(p)->kind) {
        case TK_NUM: { double v=peek(p)->num; advance(p); Node *n=mknode(ND_NUM,line); n->num=v; return n; }
        case TK_TRUE:  { advance(p); Node *n=mknode(ND_BOOL,line); n->bval=1; return n; }
        case TK_FALSE: { advance(p); Node *n=mknode(ND_BOOL,line); n->bval=0; return n; }
        case TK_NIL:   { advance(p); return mknode(ND_NIL,line); }
        case TK_STR: {
            char *s=peek(p)->str; advance(p);
            Node *n=mknode(ND_STR,line); n->str=strdup(s); return n;
        }
        case TK_FMTSTR: {
            /* encoded: text \x01 expr \x02 text \x01 expr \x02 ... */
            char *raw = peek(p)->str; advance(p);
            Node *n = mknode(ND_FMTSTR, line);
            const char *p2 = raw;
            while (*p2 || p2 == raw) {  /* always at least one iteration */
                /* collect literal segment up to \x01 */
                char *seg = NULL; int slen=0, scap=0;
                while (*p2 && (unsigned char)*p2 != 0x01) {
                    if (slen+1 >= scap) { scap=scap?scap*2:64; seg=realloc(seg,(size_t)scap); }
                    seg[slen++] = *p2++;
                }
                if (slen+1 >= scap) { scap=scap?scap*2:2; seg=realloc(seg,(size_t)scap); }
                seg[slen] = '\0';
                Node *lit = mknode(ND_STR, line); lit->str = seg ? seg : strdup("");
                nl_push(&n->fmtstr, lit);

                if ((unsigned char)*p2 != 0x01) break; /* end of string */
                p2++; /* skip \x01 */

                /* collect expression src up to \x02 */
                char *esrc = NULL; int elen=0, ecap=0;
                while (*p2 && (unsigned char)*p2 != 0x02) {
                    if (elen+1 >= ecap) { ecap=ecap?ecap*2:64; esrc=realloc(esrc,(size_t)ecap); }
                    esrc[elen++] = *p2++;
                }
                if (elen+1 >= ecap) { ecap=ecap?ecap*2:2; esrc=realloc(esrc,(size_t)ecap); }
                esrc[elen] = '\0';
                if ((unsigned char)*p2 == 0x02) p2++;

                /* parse the sub-expression */
                Lexer  *sub  = lexer_new(esrc); lexer_tokenize(sub);
                Parser  subp = {0}; subp.l = sub;
                Node   *expr = parse_expr(&subp);
                if (subp.had_error) {
                    snprintf(p->errbuf, sizeof(p->errbuf),
                        "Error in interpolation: %.300s", subp.errbuf);
                    p->had_error = 1;
                    lexer_free(sub); free(esrc); return n;
                }
                lexer_free(sub); free(esrc);
                nl_push(&n->fmtstr, expr);
            }
            return n;
        }
        case TK_IDENT: {
            char *name=peek(p)->str; advance(p);
            Node *n=mknode(ND_IDENT,line); n->str=strdup(name); return n;
        }
        case TK_SUPER: {
            advance(p);
            Node *n=mknode(ND_IDENT,line); n->str=strdup("__super__"); return n;
        }
        case TK_LPAREN: {
            advance(p);
            Node *e = parse_expr(p);
            eat(p, TK_RPAREN, ")");
            return e;
        }
        case TK_LBRACKET: {
            advance(p); skip_nl(p);
            Node *n = mknode(ND_LIST, line);
            while (!check(p,TK_RBRACKET) && !check(p,TK_EOF)) {
                skip_nl(p); nl_push(&n->list, parse_expr(p)); skip_nl(p);
                if (check(p,TK_COMMA)) advance(p);
            }
            eat(p, TK_RBRACKET, "]"); return n;
        }
        case TK_LBRACE: {
            advance(p); skip_nl(p);
            Node *n = mknode(ND_MAP, line); int cap=0;
            while (!check(p,TK_RBRACE) && !check(p,TK_EOF)) {
                skip_nl(p);
                char *key;
                if      (check(p,TK_IDENT)) key = strdup(advance(p)->str);
                else if (check(p,TK_STR))   key = strdup(advance(p)->str);
                else { p->had_error=1; snprintf(p->errbuf,sizeof(p->errbuf),"Expected map key at line %d",peek(p)->line); break; }
                eat(p, TK_COLON, "map :");
                Node *val = parse_expr(p); skip_nl(p);
                if (check(p,TK_COMMA)) advance(p);
                if (n->map.len >= cap) { cap=cap?cap*2:4; n->map.pairs=realloc(n->map.pairs,(size_t)cap*sizeof(MapPair)); }
                n->map.pairs[n->map.len].key = key;
                n->map.pairs[n->map.len].val = val;
                n->map.len++;
            }
            eat(p, TK_RBRACE, "}"); return n;
        }
        default: {
            p->had_error = 1;
            snprintf(p->errbuf, sizeof(p->errbuf),
                "Unexpected token kind=%d '%s' at line %d",
                peek(p)->kind, peek(p)->str ? peek(p)->str : "?", line);
            advance(p);
            return mknode(ND_NIL, line);
        }
    }
}

static Node *parse_postfix(Parser *p) {
    Node *e = parse_primary(p);
    while (!p->had_error) {
        int line = peek(p)->line;
        if (check(p,TK_LPAREN)) {
            advance(p); skip_nl(p);
            Node *n = mknode(ND_CALL, line); n->call.callee = e;
            while (!check(p,TK_RPAREN) && !check(p,TK_EOF)) {
                skip_nl(p); nl_push(&n->call.args, parse_expr(p)); skip_nl(p);
                if (check(p,TK_COMMA)) advance(p);
            }
            eat(p, TK_RPAREN, ")"); e = n;
        } else if (check(p,TK_LBRACKET)) {
            advance(p);
            Node *n = mknode(ND_INDEX, line);
            n->idx.obj = e; n->idx.idx = parse_expr(p);
            eat(p, TK_RBRACKET, "]"); e = n;
        } else if (check(p,TK_DOT)) {
            advance(p);
            Token *prop = eat(p, TK_IDENT, "prop");
            Node *n = mknode(ND_PROP, line);
            n->prop.obj = e; n->prop.prop = strdup(prop->str); e = n;
        } else break;
    }
    return e;
}

static Node *parse_unary(Parser *p) {
    int line = peek(p)->line;
    if (check(p,TK_MINUS)) {
        advance(p); Node *n=mknode(ND_UNOP,line); n->unop.op=strdup("-"); n->unop.expr=parse_unary(p); return n;
    }
    if (check(p,TK_NOT)) {
        advance(p); Node *n=mknode(ND_UNOP,line); n->unop.op=strdup("not"); n->unop.expr=parse_unary(p); return n;
    }
    return parse_postfix(p);
}

static Node *parse_power(Parser *p) {
    Node *l = parse_unary(p);
    while (check(p,TK_CARET)) {
        int line=peek(p)->line; advance(p);
        Node *n=mknode(ND_BINOP,line); n->binop.op=strdup("^");
        n->binop.left=l; n->binop.right=parse_unary(p); l=n;
    }
    return l;
}

static Node *parse_mul(Parser *p) {
    Node *l = parse_power(p);
    while (check(p,TK_STAR)||check(p,TK_SLASH)||check(p,TK_PERCENT)) {
        int line=peek(p)->line;
        const char *op = check(p,TK_STAR)?"*":check(p,TK_SLASH)?"/":"%";
        advance(p);
        Node *n=mknode(ND_BINOP,line); n->binop.op=strdup(op);
        n->binop.left=l; n->binop.right=parse_power(p); l=n;
    }
    return l;
}

static Node *parse_add(Parser *p) {
    Node *l = parse_mul(p);
    while (check(p,TK_PLUS)||check(p,TK_MINUS)||check(p,TK_DOTDOT)) {
        int line=peek(p)->line;
        const char *op = check(p,TK_PLUS)?"+":check(p,TK_MINUS)?"-":"..";
        advance(p);
        Node *n=mknode(ND_BINOP,line); n->binop.op=strdup(op);
        n->binop.left=l; n->binop.right=parse_mul(p); l=n;
    }
    return l;
}

static Node *parse_cmp(Parser *p) {
    Node *l = parse_add(p);
    /* handle 'is' type check */
    if (check(p,TK_IS)) {
        int line=peek(p)->line; advance(p);
        Token *cname = eat(p, TK_IDENT, "class name after is");
        Node *n = mknode(ND_BINOP, line);
        n->binop.op    = strdup("is");
        n->binop.left  = l;
        Node *cn = mknode(ND_IDENT, line); cn->str = strdup(cname->str);
        n->binop.right = cn;
        return n;
    }
    while (check(p,TK_EQ)||check(p,TK_NEQ)||check(p,TK_LT)||
           check(p,TK_GT)||check(p,TK_LE)||check(p,TK_GE)) {
        int line=peek(p)->line;
        const char *op = check(p,TK_EQ)?"==":check(p,TK_NEQ)?"!=":
                         check(p,TK_LT)?"<": check(p,TK_GT)?">":
                         check(p,TK_LE)?"<=":">=";
        advance(p);
        Node *n=mknode(ND_BINOP,line); n->binop.op=strdup(op);
        n->binop.left=l; n->binop.right=parse_add(p); l=n;
    }
    return l;
}

static Node *parse_and(Parser *p) {
    Node *l = parse_cmp(p);
    while (check(p,TK_AND)) {
        int line=peek(p)->line; advance(p);
        Node *n=mknode(ND_BINOP,line); n->binop.op=strdup("and");
        n->binop.left=l; n->binop.right=parse_cmp(p); l=n;
    }
    return l;
}

static Node *parse_or(Parser *p) {
    Node *l = parse_and(p);
    while (check(p,TK_OR)) {
        int line=peek(p)->line; advance(p);
        Node *n=mknode(ND_BINOP,line); n->binop.op=strdup("or");
        n->binop.left=l; n->binop.right=parse_and(p); l=n;
    }
    return l;
}

static Node *parse_assign(Parser *p) {
    Node *l = parse_or(p);
    int line = peek(p)->line;
    TKind k = peek(p)->kind;
    const char *op = NULL;
    if      (k==TK_ASSIGN)   op="=";
    else if (k==TK_PLUSEQ)   op="+=";
    else if (k==TK_MINUSEQ)  op="-=";
    else if (k==TK_STAREQ)   op="*=";
    else if (k==TK_SLASHEQ)  op="/=";
    if (op) {
        advance(p);
        Node *n = mknode(ND_ASSIGN, line);
        n->assign.target = l;
        n->assign.val    = parse_assign(p);
        n->assign.op     = strdup(op);
        return n;
    }
    return l;
}

/* ══════════════════════════════════════════════════════════
   ENTRY POINT
══════════════════════════════════════════════════════════ */
Node *parse(Lexer *l) {
    Parser p = {0}; p.l = l;
    Node *prog = mknode(ND_PROGRAM, 1);
    skip_nl(&p);
    while (!check(&p,TK_EOF)) {
        if (check(&p,TK_NEWLINE)) { advance(&p); continue; }
        nl_push(&prog->block, parse_stmt(&p));
        if (p.had_error) {
            fprintf(stderr, "Parse error: %s\n", p.errbuf);
            node_free(prog); return NULL;
        }
        skip_nl(&p);
    }
    return prog;
}
