#ifndef KITE_H
#define KITE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <ctype.h>

typedef struct Value    Value;
typedef struct Env      Env;
typedef struct Node     Node;
typedef struct NodeList NodeList;
typedef struct KiteClass    KiteClass;
typedef struct KiteInstance KiteInstance;

/* ══════════════════════════════════════════════════════════
   TOKENS
══════════════════════════════════════════════════════════ */
typedef enum {
    TK_NUM, TK_STR, TK_FMTSTR, TK_IDENT,
    TK_SET, TK_DEF, TK_GIVE, TK_END,
    TK_WHEN, TK_ORWHEN, TK_ELSE,
    TK_LOOP, TK_WHILE, TK_FOR, TK_IN,
    TK_DO, TK_ERR,
    TK_OBJ, TK_EXTENDS, TK_IS, TK_SUPER,
    TK_TRUE, TK_FALSE, TK_NIL,
    TK_AND, TK_OR, TK_NOT,
    TK_BREAK, TK_NEXT, TK_USE,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT, TK_CARET,
    TK_EQ, TK_NEQ, TK_LT, TK_GT, TK_LE, TK_GE,
    TK_ASSIGN, TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ,
    TK_ARROW, TK_DOTDOT,
    TK_LPAREN, TK_RPAREN,
    TK_LBRACKET, TK_RBRACKET,
    TK_LBRACE, TK_RBRACE,
    TK_COMMA, TK_COLON, TK_DOT,
    TK_NEWLINE, TK_EOF
} TKind;

typedef struct {
    TKind  kind;
    char  *str;
    double num;
    int    line;
} Token;

typedef struct {
    const char *src;
    int         pos;
    int         line;
    Token      *buf;
    int         buf_cap;
    int         buf_len;
} Lexer;

/* ══════════════════════════════════════════════════════════
   AST
══════════════════════════════════════════════════════════ */
typedef enum {
    ND_NUM, ND_STR, ND_FMTSTR, ND_BOOL, ND_NIL,
    ND_IDENT,
    ND_SET,
    ND_ASSIGN,
    ND_BINOP,
    ND_UNOP,
    ND_CALL,
    ND_INDEX,
    ND_PROP,
    ND_LIST,
    ND_MAP,
    ND_LAMBDA,
    ND_DEF,
    ND_WHEN,
    ND_LOOP_WHILE,
    ND_LOOP_FOR,
    ND_OBJ,       /* obj Name [extends Base]: ... end */
    ND_DO,        /* do: body  err ErrType: handler  end */
    ND_GIVE,
    ND_BREAK,
    ND_NEXT,
    ND_USE,
    ND_BLOCK,
    ND_EXPR_STMT,
    ND_PROGRAM
} NKind;

struct NodeList {
    Node **items;
    int    len;
    int    cap;
};

typedef struct { char *key; Node *val; } MapPair;

/* err handler: err_type == NULL means catch-all */
typedef struct {
    char *err_type;   /* "ZeroDivisionError", "TypeError", NULL = any */
    Node *body;
} ErrHandler;

struct Node {
    NKind kind;
    int   line;
    union {
        double  num;
        char   *str;
        int     bval;
        struct { char *name; Node *val; }                         set;
        struct { Node *target; Node *val; char *op; }             assign;
        struct { char *op; Node *left; Node *right; }             binop;
        struct { char *op; Node *expr; }                          unop;
        struct { Node *callee; NodeList args; }                   call;
        struct { Node *obj; Node *idx; }                          idx;
        struct { Node *obj; char *prop; }                         prop;
        NodeList list;
        NodeList fmtstr;
        struct { MapPair *pairs; int len; }                       map;
        struct { char **params; int nparams; Node *body; }        lambda;
        struct { char *name; char **params; int nparams;
                 Node *body; }                                     def;
        struct { Node **conds; Node **bodies; int nbranch; }      when;
        struct { Node *cond; Node *body; }                        loop_while;
        struct { char *var; Node *iter; Node *body; }             loop_for;
        struct {
            char    *name;
            char    *parent;  /* NULL if no extends */
            NodeList body;
        }                                                          obj_def;
        struct {
            Node        *body;
            ErrHandler  *handlers;
            int          nhandlers;
        }                                                          do_catch;
        Node *child;
        NodeList block;
    };
};

/* ── OOP ─────────────────────────────────────────────── */
typedef struct KiteClass {
    char     *name;
    struct KiteClass *parent;   /* superclass or NULL */
    Env      *methods;          /* method env: name -> VT_FN */
    int       refs;
} KiteClass;

typedef struct KiteInstance {
    KiteClass *klass;
    Env       *fields;          /* instance fields */
    int        refs;
} KiteInstance;


/* ══════════════════════════════════════════════════════════
   VALUES
══════════════════════════════════════════════════════════ */
typedef enum {
    VT_NIL, VT_BOOL, VT_NUM, VT_STR,
    VT_LIST, VT_MAP, VT_FN, VT_BUILTIN,
    VT_CLASS, VT_INSTANCE
} VType;

typedef struct KiteList {
    Value **items;
    int     len;
    int     cap;
    int     refs;
} KiteList;

typedef struct KiteMap {
    char  **keys;
    Value **vals;
    int     len;
    int     cap;
    int     refs;
} KiteMap;

typedef struct KiteFn {
    char  *name;
    char **params;
    int    nparams;
    Node  *body;
    Env   *closure;
    int    refs;
} KiteFn;

typedef Value *(*BuiltinFn)(Value **args, int nargs);

struct Value {
    VType type;
    int   refs;
    union {
        int       bval;
        double    num;
        char     *str;
        KiteList *list;
        KiteMap  *map;
        KiteFn       *fn;
        KiteClass    *klass;
        KiteInstance *instance;
        struct { BuiltinFn fn; const char *name; } builtin;
    };
};

/* ══════════════════════════════════════════════════════════
   ENVIRONMENT
══════════════════════════════════════════════════════════ */
#define ENV_HASH 64
typedef struct EnvBucket {
    char *name; Value *val; struct EnvBucket *next;
} EnvBucket;

struct Env {
    EnvBucket *buckets[ENV_HASH];
    Env       *parent;
    int        refs;
};

/* ══════════════════════════════════════════════════════════
   INTERPRETER STATE
══════════════════════════════════════════════════════════ */
typedef struct {
    int       has_return;
    Value    *retval;
    int       has_break;
    int       has_next;
    char      errbuf[512];
    char      errtype[64];   /* error type: "ZeroDivisionError" etc */
    int       had_error;
    long long steps;
    KiteClass *current_class;  /* set when executing a method */
    char script_dir[512];       /* directory of running script for use mymodule */
} Interp;

/* ══════════════════════════════════════════════════════════
   PROTOTYPES
══════════════════════════════════════════════════════════ */
Lexer *lexer_new(const char *src);
void   lexer_free(Lexer *l);
void   lexer_tokenize(Lexer *l);

Node  *parse(Lexer *l);
void   node_free(Node *n);

Value *val_nil(void);
Value *val_bool(int b);
Value *val_num(double n);
Value *val_str(const char *s);
Value *val_list(void);
Value *val_map(void);
Value *val_fn(KiteFn *fn);
Value *val_builtin(BuiltinFn fn, const char *name);
Value *val_class(KiteClass *klass);
Value *val_instance(KiteInstance *inst);
Value *val_ref(Value *v);
void   val_unref(Value *v);
char  *val_tostr(Value *v);
int    val_truthy(Value *v);
int    val_eq(Value *a, Value *b);
void   val_print(Value *v, FILE *f);

Env   *env_new(Env *parent);
void   env_ref(Env *e);
void   env_unref(Env *e);
Value *env_get(Env *e, const char *name);
int    env_set(Env *e, const char *name, Value *v);
void   env_def(Env *e, const char *name, Value *v);

Interp *interp_new(void);
void    interp_free(Interp *ip);
void    setup_builtins(Env *env);
Value  *eval(Interp *ip, Node *n, Env *env);
void    kite_error(Interp *ip, int line, const char *type, const char *fmt, ...);

#endif
