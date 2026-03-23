#include "kite.h"

/* ══════════════════════════════════════════════════════════
   VALUE CONSTRUCTORS
══════════════════════════════════════════════════════════ */
static Value *val_alloc(VType t) {
    Value *v = calloc(1, sizeof(Value));
    v->type  = t;
    v->refs  = 1;
    return v;
}

Value *val_nil(void)     { return val_alloc(VT_NIL); }
Value *val_bool(int b)   { Value *v=val_alloc(VT_BOOL); v->bval=b?1:0; return v; }
Value *val_num(double n) { Value *v=val_alloc(VT_NUM);  v->num=n;       return v; }

Value *val_str(const char *s) {
    Value *v = val_alloc(VT_STR);
    v->str   = strdup(s ? s : "");
    return v;
}

Value *val_list(void) {
    Value *v    = val_alloc(VT_LIST);
    v->list     = calloc(1, sizeof(KiteList));
    v->list->refs = 1;
    return v;
}

Value *val_map(void) {
    Value *v   = val_alloc(VT_MAP);
    v->map     = calloc(1, sizeof(KiteMap));
    v->map->refs = 1;
    return v;
}

Value *val_class(KiteClass *klass) {
    Value *v   = val_alloc(VT_CLASS);
    v->klass   = klass;
    klass->refs++;
    return v;
}
Value *val_instance(KiteInstance *inst) {
    Value *v    = val_alloc(VT_INSTANCE);
    v->instance = inst;
    inst->refs++;
    return v;
}
Value *val_fn(KiteFn *fn) {
    Value *v = val_alloc(VT_FN);
    v->fn    = fn;
    fn->refs++;
    return v;
}

Value *val_builtin(BuiltinFn fn, const char *name) {
    Value *v          = val_alloc(VT_BUILTIN);
    v->builtin.fn     = fn;
    v->builtin.name   = name;
    return v;
}

/* ══════════════════════════════════════════════════════════
   REFERENCE COUNTING
══════════════════════════════════════════════════════════ */
Value *val_ref(Value *v) { if (v) v->refs++; return v; }

static void list_unref(KiteList *l) {
    if (!l || --l->refs > 0) return;
    for (int i = 0; i < l->len; i++) val_unref(l->items[i]);
    free(l->items); free(l);
}

static void map_unref(KiteMap *m) {
    if (!m || --m->refs > 0) return;
    for (int i = 0; i < m->len; i++) { free(m->keys[i]); val_unref(m->vals[i]); }
    free(m->keys); free(m->vals); free(m);
}

static void fn_unref(KiteFn *f) {
    if (!f || --f->refs > 0) return;
    free(f->name);
    for (int i = 0; i < f->nparams; i++) free(f->params[i]);
    free(f->params);
    env_unref(f->closure);
    free(f);
}

static void class_unref(KiteClass *k) {
    if (!k || --k->refs > 0) return;
    free(k->name);
    /* parent ref managed separately */
    env_unref(k->methods);
    free(k);
}
static void instance_unref(KiteInstance *inst) {
    if (!inst || --inst->refs > 0) return;
    /* klass ref: don't free klass here, it lives in env */
    env_unref(inst->fields);
    free(inst);
}
void val_unref(Value *v) {
    if (!v || --v->refs > 0) return;
    switch (v->type) {
        case VT_STR:      free(v->str);           break;
        case VT_LIST:     list_unref(v->list);     break;
        case VT_MAP:      map_unref(v->map);       break;
        case VT_FN:       fn_unref(v->fn);         break;
        case VT_CLASS:    class_unref(v->klass);   break;
        case VT_INSTANCE: instance_unref(v->instance); break;
        default: break;
    }
    free(v);
}

/* ══════════════════════════════════════════════════════════
   VALUE UTILITIES
══════════════════════════════════════════════════════════ */
int val_truthy(Value *v) {
    if (!v) return 0;
    switch (v->type) {
        case VT_NIL:  return 0;
        case VT_BOOL: return v->bval;
        case VT_NUM:  return v->num != 0.0;
        case VT_STR:  return v->str[0] != '\0';
        case VT_LIST:     return v->list->len > 0;
        case VT_INSTANCE: return 1;
        default:          return 1;
    }
}

int val_eq(Value *a, Value *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return 0;
    switch (a->type) {
        case VT_NIL:  return 1;
        case VT_BOOL: return a->bval == b->bval;
        case VT_NUM:  return a->num  == b->num;
        case VT_STR:  return strcmp(a->str, b->str) == 0;
        case VT_LIST: {
            if (a->list->len != b->list->len) return 0;
            for (int i=0; i<a->list->len; i++)
                if (!val_eq(a->list->items[i], b->list->items[i])) return 0;
            return 1;
        }
        default: return a == b;
    }
}

/* ── String builder ────────────────────────────────────── */
typedef struct { char *buf; size_t len; size_t cap; } SB;

static void sb_push(SB *sb, const char *s) {
    size_t n = strlen(s);
    if (sb->len + n + 1 > sb->cap) {
        sb->cap = (sb->cap ? sb->cap * 2 : 64) + n;
        sb->buf = realloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, s, n + 1);
    sb->len += n;
}

static void sb_pushc(SB *sb, char c) { char tmp[2]={c,0}; sb_push(sb,tmp); }

static void sb_pushf(SB *sb, const char *fmt, ...) {
    char tmp[128];
    va_list ap; va_start(ap,fmt); vsnprintf(tmp,sizeof(tmp),fmt,ap); va_end(ap);
    sb_push(sb,tmp);
}

/* repr: depth>0 means strings get quotes (for inside lists/maps) */
static void repr(Value *v, SB *sb, int depth) {
    if (!v) { sb_push(sb,"nil"); return; }
    switch (v->type) {
        case VT_NIL:  sb_push(sb,"nil"); break;
        case VT_BOOL: sb_push(sb, v->bval ? "true" : "false"); break;
        case VT_NUM: {
            double n = v->num;
            if (n == (long long)n) sb_pushf(sb,"%lld",(long long)n);
            else                   sb_pushf(sb,"%g",n);
            break;
        }
        case VT_STR:
            if (depth > 0) { sb_pushc(sb,'"'); sb_push(sb,v->str); sb_pushc(sb,'"'); }
            else             sb_push(sb, v->str);
            break;
        case VT_LIST:
            sb_push(sb,"[");
            for (int i=0; i<v->list->len; i++) {
                if (i) sb_push(sb,", ");
                repr(v->list->items[i], sb, depth+1);
            }
            sb_push(sb,"]");
            break;
        case VT_MAP:
            sb_push(sb,"{");
            for (int i=0; i<v->map->len; i++) {
                if (i) sb_push(sb,", ");
                sb_push(sb, v->map->keys[i]);
                sb_push(sb,": ");
                repr(v->map->vals[i], sb, depth+1);
            }
            sb_push(sb,"}");
            break;
        case VT_FN:
            sb_pushf(sb,"<fn %s>", v->fn->name ? v->fn->name : "λ");
            break;
        case VT_BUILTIN:
            sb_pushf(sb,"<builtin %s>", v->builtin.name);
            break;
        case VT_CLASS:
            sb_pushf(sb,"<class %s>", v->klass->name);
            break;
        case VT_INSTANCE: {
            /* look for str() method */
            sb_pushf(sb,"<%s instance>", v->instance->klass->name);
            break;
        }
    }
}

/* val_tostr: always without quotes on outer string (for say and interpolation) */
char *val_tostr(Value *v) {
    SB sb = {NULL,0,0};
    repr(v, &sb, 0);
    if (!sb.buf) sb.buf = strdup("");
    return sb.buf;
}

/* val_print: same as val_tostr but writes to FILE */
void val_print(Value *v, FILE *f) {
    char *s = val_tostr(v);
    fputs(s, f);
    free(s);
}

/* ══════════════════════════════════════════════════════════
   ENVIRONMENT
══════════════════════════════════════════════════════════ */
static unsigned env_hash(const char *name) {
    unsigned h = 5381;
    while (*name) h = h * 33 ^ (unsigned char)*name++;
    return h % ENV_HASH;
}

Env *env_new(Env *parent) {
    Env *e    = calloc(1, sizeof(Env));
    e->parent = parent;
    e->refs   = 1;
    if (parent) env_ref(parent);
    return e;
}

void env_ref(Env *e)  { if (e) e->refs++; }

void env_unref(Env *e) {
    if (!e || --e->refs > 0) return;
    for (int i=0; i<ENV_HASH; i++) {
        EnvBucket *b = e->buckets[i];
        while (b) {
            EnvBucket *nx = b->next;
            free(b->name); val_unref(b->val); free(b);
            b = nx;
        }
    }
    env_unref(e->parent);
    free(e);
}

void env_def(Env *e, const char *name, Value *v) {
    unsigned h = env_hash(name);
    for (EnvBucket *b=e->buckets[h]; b; b=b->next) {
        if (strcmp(b->name, name)==0) { val_unref(b->val); b->val=val_ref(v); return; }
    }
    EnvBucket *nb = malloc(sizeof(EnvBucket));
    nb->name = strdup(name); nb->val = val_ref(v);
    nb->next = e->buckets[h]; e->buckets[h] = nb;
}

Value *env_get(Env *e, const char *name) {
    for (Env *cur=e; cur; cur=cur->parent) {
        unsigned h = env_hash(name);
        for (EnvBucket *b=cur->buckets[h]; b; b=b->next)
            if (strcmp(b->name, name)==0) return b->val;
    }
    return NULL;
}

int env_set(Env *e, const char *name, Value *v) {
    for (Env *cur=e; cur; cur=cur->parent) {
        unsigned h = env_hash(name);
        for (EnvBucket *b=cur->buckets[h]; b; b=b->next) {
            if (strcmp(b->name, name)==0) {
                val_unref(b->val); b->val=val_ref(v); return 1;
            }
        }
    }
    return 0;
}
