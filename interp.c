#include "kite.h"

#define MAX_STEPS 5000000LL

Interp *interp_new(void)  { return calloc(1, sizeof(Interp)); }
void    interp_free(Interp *ip) { free(ip); }

void kite_error(Interp *ip, int line, const char *type, const char *fmt, ...) {
    ip->had_error = 1;
    if (type) snprintf(ip->errtype, sizeof(ip->errtype), "%s", type);
    else       snprintf(ip->errtype, sizeof(ip->errtype), "Error");
    char tmp[400]; va_list ap; va_start(ap,fmt); vsnprintf(tmp,sizeof(tmp),fmt,ap); va_end(ap);
    if (line > 0)
        snprintf(ip->errbuf, sizeof(ip->errbuf), "line %d: %s", line, tmp);
    else
        snprintf(ip->errbuf, sizeof(ip->errbuf), "%s", tmp);
}

/* ── Global interp pointer for builtins that call user functions ── */
static Interp *g_ip = NULL;

/* ══════════════════════════════════════════════════════════
   HELPERS
══════════════════════════════════════════════════════════ */
#define BUILTIN_ERR(msg) \
    do { if (g_ip) { kite_error(g_ip, 0, "RuntimeError", "%s", msg); } return val_nil(); } while(0)

static Value *list_push_val(Value *lst, Value *item) {
    if (lst->type != VT_LIST) { BUILTIN_ERR("push: expected list"); }
    KiteList *l = lst->list;
    if (l->len >= l->cap) {
        l->cap = l->cap ? l->cap*2 : 8;
        l->items = realloc(l->items, (size_t)l->cap * sizeof(Value*));
    }
    l->items[l->len++] = val_ref(item);
    return val_ref(lst);
}

static Value *call_fn(Interp *ip, Value *fn, Value **args, int nargs, int line);
Value *eval(Interp *ip, Node *n, Env *env);

static Value *eval_block(Interp *ip, NodeList *stmts, Env *env) {
    Value *last = val_nil();
    for (int i=0; i<stmts->len && !ip->had_error; i++) {
        val_unref(last);
        last = eval(ip, stmts->items[i], env);
        if (ip->has_return || ip->has_break || ip->has_next) break;
    }
    return last;
}

/* ══════════════════════════════════════════════════════════
   BUILT-IN FUNCTIONS
══════════════════════════════════════════════════════════ */

/* I/O */
static Value *bi_talk(Value **args, int nargs) {
    if (nargs < 1) BUILTIN_ERR("talk: expected arg");
    char *s = val_tostr(args[0]);
    fputs(s, stdout);
    fflush(stdout);
    free(s);
    return val_nil();
}

static Value *bi_say(Value **args, int nargs) {
    for (int i=0;i<nargs;i++) {
        if (i) putchar(' ');
        val_print(args[i], stdout);
    }
    putchar('\n');
    return val_nil();
}
static Value *bi_input(Value **args, int nargs) {
    if (nargs >= 1) { char *s=val_tostr(args[0]); printf("%s",s); fflush(stdout); free(s); }
    char buf[4096]; buf[0]='\0';
    if (fgets(buf, (int)sizeof(buf), stdin)) {
        int n = (int)strlen(buf);
        if (n>0 && buf[n-1]=='\n') buf[n-1]='\0';
    }
    return val_str(buf);
}

/* Types */
static Value *bi_type(Value **args, int nargs) {
    if (nargs<1) return val_str("nil");
    switch (args[0]->type) {
        case VT_NIL:     return val_str("nil");
        case VT_BOOL:    return val_str("bool");
        case VT_NUM:     return val_str("num");
        case VT_STR:     return val_str("str");
        case VT_LIST:    return val_str("list");
        case VT_MAP:     return val_str("map");
        case VT_FN:      return val_str("fn");
        case VT_BUILTIN: return val_str("builtin");
        case VT_CLASS:   return val_str("class");
        case VT_INSTANCE: {
            /* return class name as type */
            return val_str(args[0]->instance->klass->name);
        }
    }
    return val_str("?");
}
static Value *bi_str_conv(Value **args, int nargs) {
    if (nargs<1) return val_str("");
    char *s = val_tostr(args[0]); Value *v = val_str(s); free(s); return v;
}
static Value *bi_num_conv(Value **args, int nargs) {
    if (nargs<1) BUILTIN_ERR("num: expected arg");
    if (args[0]->type==VT_NUM)  return val_ref(args[0]);
    if (args[0]->type==VT_BOOL) return val_num(args[0]->bval);
    if (args[0]->type==VT_STR)  return val_num(atof(args[0]->str));
    BUILTIN_ERR("num: cannot convert");
}

/* Collections */
static Value *bi_len(Value **args, int nargs) {
    if (nargs<1) BUILTIN_ERR("len: expected arg");
    if (args[0]->type==VT_STR)  return val_num((double)strlen(args[0]->str));
    if (args[0]->type==VT_LIST) return val_num((double)args[0]->list->len);
    if (args[0]->type==VT_MAP)  return val_num((double)args[0]->map->len);
    BUILTIN_ERR("len: unsupported type");
}
static Value *bi_push(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("push: expected (list, val)");
    return list_push_val(args[0], args[1]);
}
static Value *bi_added(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST) BUILTIN_ERR("added: expected (list, val)");
    /* return new list with val appended, original unchanged */
    Value *res = val_list();
    KiteList *l = args[0]->list;
    for (int i=0;i<l->len;i++) list_push_val(res, l->items[i]);
    list_push_val(res, args[1]);
    return res;
}

static Value *bi_pop(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_LIST) BUILTIN_ERR("pop: expected list");
    KiteList *l = args[0]->list;
    if (l->len==0) return val_nil();
    return l->items[--l->len]; /* hand off ref */
}
static Value *bi_keys(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_MAP) BUILTIN_ERR("keys: expected map");
    Value *res = val_list();
    KiteMap *m = args[0]->map;
    for (int i=0;i<m->len;i++) {
        Value *k = val_str(m->keys[i]);
        list_push_val(res, k); val_unref(k);
    }
    return res;
}
static Value *bi_vals(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_MAP) BUILTIN_ERR("vals: expected map");
    Value *res = val_list();
    KiteMap *m = args[0]->map;
    for (int i=0;i<m->len;i++) list_push_val(res, m->vals[i]);
    return res;
}
static Value *bi_has(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("has: expected 2 args");
    Value *col=args[0], *key=args[1];
    if (col->type==VT_LIST) {
        for (int i=0;i<col->list->len;i++) if (val_eq(col->list->items[i],key)) return val_bool(1);
        return val_bool(0);
    }
    if (col->type==VT_MAP) {
        if (key->type!=VT_STR) BUILTIN_ERR("has: map key must be str");
        for (int i=0;i<col->map->len;i++) if (strcmp(col->map->keys[i],key->str)==0) return val_bool(1);
        return val_bool(0);
    }
    if (col->type==VT_STR && key->type==VT_STR) return val_bool(strstr(col->str,key->str)!=NULL);
    BUILTIN_ERR("has: unsupported types");
}
static Value *bi_range(Value **args, int nargs) {
    double start=0,end_=0,step=1;
    if      (nargs==1) { end_=args[0]->num; }
    else if (nargs==2) { start=args[0]->num; end_=args[1]->num; }
    else if (nargs>=3) { start=args[0]->num; end_=args[1]->num; step=args[2]->num; }
    if (step==0) BUILTIN_ERR("range: step cannot be 0");
    Value *res = val_list();
    if (step>0) { for (double i=start;i<end_;i+=step) { Value *v=val_num(i); list_push_val(res,v); val_unref(v); } }
    else        { for (double i=start;i>end_;i+=step) { Value *v=val_num(i); list_push_val(res,v); val_unref(v); } }
    return res;
}
static Value *bi_slice(Value **args, int nargs) {
    if (nargs<1) BUILTIN_ERR("slice: expected args");
    Value *v=args[0];
    int start = nargs>=2 ? (int)args[1]->num : 0;
    int end_;
    if (v->type==VT_LIST) {
        int ln=v->list->len;
        if (start<0) { start=ln+start; }
        if (start<0) { start=0; }
        end_ = nargs>=3 ? (int)args[2]->num : ln;
        if (end_<0)  { end_=ln+end_; }
        if (end_>ln) { end_=ln; }
        Value *res=val_list();
        for (int i=start;i<end_;i++) list_push_val(res, v->list->items[i]);
        return res;
    }
    if (v->type==VT_STR) {
        int ln=(int)strlen(v->str);
        if (start<0) { start=ln+start; }
        if (start<0) { start=0; }
        end_ = nargs>=3 ? (int)args[2]->num : ln;
        if (end_<0)  { end_=ln+end_; }
        if (end_>ln) { end_=ln; }
        if (end_<=start) return val_str("");
        char *buf=malloc((size_t)(end_-start)+1);
        memcpy(buf, v->str+start, (size_t)(end_-start));
        buf[end_-start]='\0';
        Value *res=val_str(buf); free(buf); return res;
    }
    BUILTIN_ERR("slice: expected list or str");
}
static Value *bi_reverse(Value **args, int nargs) {
    if (nargs<1) BUILTIN_ERR("reverse: expected arg");
    if (args[0]->type==VT_LIST) {
        Value *res=val_list(); KiteList *l=args[0]->list;
        for (int i=l->len-1;i>=0;i--) list_push_val(res,l->items[i]);
        return res;
    }
    if (args[0]->type==VT_STR) {
        int ln=(int)strlen(args[0]->str);
        char *buf=malloc((size_t)ln+1);
        for (int i=0;i<ln;i++) buf[i]=args[0]->str[ln-1-i];
        buf[ln]='\0'; Value *v=val_str(buf); free(buf); return v;
    }
    BUILTIN_ERR("reverse: expected list or str");
}
static Value *bi_concat(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST||args[1]->type!=VT_LIST)
        BUILTIN_ERR("concat: expected 2 lists");
    Value *res=val_list();
    KiteList *a=args[0]->list, *b=args[1]->list;
    for (int i=0;i<a->len;i++) list_push_val(res,a->items[i]);
    for (int i=0;i<b->len;i++) list_push_val(res,b->items[i]);
    return res;
}

/* Math */
static Value *bi_floor(Value **a, int n) { if(n<1)BUILTIN_ERR("floor: need num"); return val_num(floor(a[0]->num)); }
static Value *bi_ceil (Value **a, int n) { if(n<1)BUILTIN_ERR("ceil: need num");  return val_num(ceil (a[0]->num)); }
static Value *bi_round(Value **a, int n) { if(n<1)BUILTIN_ERR("round: need num"); return val_num(round(a[0]->num)); }
static Value *bi_sqrt (Value **a, int n) { if(n<1)BUILTIN_ERR("sqrt: need num");  return val_num(sqrt (a[0]->num)); }
static Value *bi_abs  (Value **a, int n) { if(n<1)BUILTIN_ERR("abs: need num");   return val_num(fabs (a[0]->num)); }
static Value *bi_sin  (Value **a, int n) { if(n<1)BUILTIN_ERR("sin: need num");   return val_num(sin  (a[0]->num)); }
static Value *bi_cos  (Value **a, int n) { if(n<1)BUILTIN_ERR("cos: need num");   return val_num(cos  (a[0]->num)); }
static Value *bi_log  (Value **a, int n) { if(n<1)BUILTIN_ERR("log: need num");   return val_num(log  (a[0]->num)); }

static Value *bi_min(Value **args, int nargs) {
    if (nargs==1 && args[0]->type==VT_LIST) {
        KiteList *l=args[0]->list; if(l->len==0) return val_nil();
        Value *m=l->items[0];
        for(int i=1;i<l->len;i++) if(l->items[i]->num < m->num) m=l->items[i];
        return val_ref(m);
    }
    if (nargs<1) BUILTIN_ERR("min: expected args");
    Value *m=args[0]; for(int i=1;i<nargs;i++) if(args[i]->num<m->num) m=args[i];
    return val_ref(m);
}
static Value *bi_max(Value **args, int nargs) {
    if (nargs==1 && args[0]->type==VT_LIST) {
        KiteList *l=args[0]->list; if(l->len==0) return val_nil();
        Value *m=l->items[0];
        for(int i=1;i<l->len;i++) if(l->items[i]->num > m->num) m=l->items[i];
        return val_ref(m);
    }
    if (nargs<1) BUILTIN_ERR("max: expected args");
    Value *m=args[0]; for(int i=1;i<nargs;i++) if(args[i]->num>m->num) m=args[i];
    return val_ref(m);
}

/* Strings */
static Value *bi_split(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("split: expected str");
    const char *sep = (nargs>=2&&args[1]->type==VT_STR) ? args[1]->str : " ";
    Value *res = val_list();
    if (strlen(sep)==0) {
        char ch[2]={0,0};
        for (const char *p=args[0]->str; *p; p++) {
            ch[0]=*p; Value *v=val_str(ch); list_push_val(res,v); val_unref(v);
        }
    } else {
        char *copy=strdup(args[0]->str), *tok=strtok(copy,sep);
        while(tok) { Value *v=val_str(tok); list_push_val(res,v); val_unref(v); tok=strtok(NULL,sep); }
        free(copy);
    }
    return res;
}
static Value *bi_join(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_LIST) BUILTIN_ERR("join: expected list");
    const char *sep = (nargs>=2&&args[1]->type==VT_STR) ? args[1]->str : "";
    KiteList *l=args[0]->list;
    /* compute total length */
    size_t total=0;
    for (int i=0;i<l->len;i++) { char *s=val_tostr(l->items[i]); total+=strlen(s)+strlen(sep); free(s); }
    char *buf=calloc(1,total+1);
    for (int i=0;i<l->len;i++) {
        if (i) strcat(buf,sep);
        char *s=val_tostr(l->items[i]); strcat(buf,s); free(s);
    }
    Value *v=val_str(buf); free(buf); return v;
}
static Value *bi_upcase(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("upcase: expected str");
    char *s=strdup(args[0]->str);
    for (int i=0;s[i];i++) s[i]=(char)toupper((unsigned char)s[i]);
    Value *v=val_str(s); free(s); return v;
}
static Value *bi_downcase(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("downcase: expected str");
    char *s=strdup(args[0]->str);
    for (int i=0;s[i];i++) s[i]=(char)tolower((unsigned char)s[i]);
    Value *v=val_str(s); free(s); return v;
}
static Value *bi_trim(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("trim: expected str");
    const char *s=args[0]->str;
    while (*s==' '||*s=='\t'||*s=='\n'||*s=='\r') s++;
    int ln=(int)strlen(s);
    while (ln>0 && (s[ln-1]==' '||s[ln-1]=='\t'||s[ln-1]=='\n'||s[ln-1]=='\r')) ln--;
    char *buf=malloc((size_t)ln+1); memcpy(buf,s,(size_t)ln); buf[ln]='\0';
    Value *v=val_str(buf); free(buf); return v;
}
static Value *bi_index_of(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("index_of: expected 2 args");
    if (args[0]->type==VT_STR&&args[1]->type==VT_STR) {
        char *p=strstr(args[0]->str,args[1]->str);
        return p ? val_num((double)(p-args[0]->str)) : val_num(-1);
    }
    if (args[0]->type==VT_LIST) {
        for (int i=0;i<args[0]->list->len;i++) if(val_eq(args[0]->list->items[i],args[1])) return val_num((double)i);
        return val_num(-1);
    }
    BUILTIN_ERR("index_of: unsupported type");
}
static Value *bi_replace(Value **args, int nargs) {
    if (nargs<3||args[0]->type!=VT_STR||args[1]->type!=VT_STR||args[2]->type!=VT_STR)
        BUILTIN_ERR("replace: expected (str, from, to)");
    const char *src=args[0]->str, *from=args[1]->str, *to=args[2]->str;
    int flen=(int)strlen(from); if(flen==0) return val_ref(args[0]);
    int tlen=(int)strlen(to);
    size_t cap=(strlen(src)+1)*(size_t)(tlen/flen+2)+2;
    char *buf=malloc(cap); const char *p=src; char *q=buf;
    while (*p) {
        if (strncmp(p,from,(size_t)flen)==0) { memcpy(q,to,(size_t)tlen); q+=tlen; p+=flen; }
        else *q++=*p++;
    }
    *q='\0'; Value *v=val_str(buf); free(buf); return v;
}

/* HOF helpers — call a user fn with 1 or 2 args */
static Value *call_user(Value *fn, Value *a, Value *b) {
    if (!g_ip) return val_nil();
    if (fn->type==VT_BUILTIN) {
        if (b) { Value *args[2]={a,b}; return fn->builtin.fn(args,2); }
        else   { Value *args[1]={a};   return fn->builtin.fn(args,1); }
    }
    if (fn->type!=VT_FN) return val_nil();
    KiteFn *kf=fn->fn;
    Env *fenv=env_new(kf->closure);
    if (kf->nparams>=1) env_def(fenv,kf->params[0],a);
    if (b && kf->nparams>=2) env_def(fenv,kf->params[1],b);
    Value *last=eval(g_ip,kf->body,fenv);
    env_unref(fenv);
    if (g_ip->has_return) {
        val_unref(last);
        Value *rv=g_ip->retval ? g_ip->retval : val_nil();
        g_ip->has_return=0; g_ip->retval=NULL;
        return rv;
    }
    return last;
}

static Value *bi_map_fn(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST) BUILTIN_ERR("map: expected (list, fn)");
    Value *res=val_list(); KiteList *l=args[0]->list; Value *fn=args[1];
    for (int i=0;i<l->len;i++) {
        Value *v=call_user(fn,l->items[i],NULL);
        if (!v) v=val_nil();
        list_push_val(res,v); val_unref(v);
    }
    return res;
}
static Value *bi_filter_fn(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST) BUILTIN_ERR("filter: expected (list, fn)");
    Value *res=val_list(); KiteList *l=args[0]->list; Value *fn=args[1];
    for (int i=0;i<l->len;i++) {
        Value *cond=call_user(fn,l->items[i],NULL);
        if (!cond) cond=val_nil();
        if (val_truthy(cond)) list_push_val(res,l->items[i]);
        val_unref(cond);
    }
    return res;
}
static Value *bi_reduce_fn(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST) BUILTIN_ERR("reduce: expected (list, fn [, init])");
    KiteList *l=args[0]->list; Value *fn=args[1];
    Value *acc;
    int start;
    if (nargs>=3) { acc=val_ref(args[2]); start=0; }
    else if (l->len>0) { acc=val_ref(l->items[0]); start=1; }
    else return val_nil();
    for (int i=start;i<l->len;i++) {
        Value *nv=call_user(fn,acc,l->items[i]);
        val_unref(acc); acc=nv ? nv : val_nil();
    }
    return acc;
}
static Value *bi_sort_fn(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_LIST) BUILTIN_ERR("sort: expected list");
    Value *fn = nargs>=2 ? args[1] : NULL;
    KiteList *l=args[0]->list;
    Value *res=val_list();
    for (int i=0;i<l->len;i++) list_push_val(res,l->items[i]);
    /* insertion sort */
    KiteList *rl=res->list;
    for (int i=1;i<rl->len;i++) {
        Value *key=rl->items[i]; int j=i-1;
        while (j>=0) {
            int gt=0;
            if (fn) {
                Value *r=call_user(fn,rl->items[j],key);
                if (r) { gt=(r->type==VT_NUM) ? r->num>0 : val_truthy(r); val_unref(r); }
            } else {
                Value *a=rl->items[j];
                if      (a->type==VT_NUM && key->type==VT_NUM) gt=a->num>key->num;
                else if (a->type==VT_STR && key->type==VT_STR) gt=strcmp(a->str,key->str)>0;
            }
            if (!gt) break;
            rl->items[j+1]=rl->items[j]; j--;
        }
        rl->items[j+1]=key;
    }
    return res;
}

static Value *bi_str_start(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_STR||args[1]->type!=VT_STR)
        BUILTIN_ERR("str_start: expected (str, prefix)");
    const char *s   = args[0]->str;
    const char *pre = args[1]->str;
    size_t slen = strlen(s), plen = strlen(pre);
    if (plen > slen) return val_bool(0);
    return val_bool(strncmp(s, pre, plen) == 0);
}

static Value *bi_str_end(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_STR||args[1]->type!=VT_STR)
        BUILTIN_ERR("str_end: expected (str, suffix)");
    const char *s   = args[0]->str;
    const char *suf = args[1]->str;
    size_t slen = strlen(s), suflen = strlen(suf);
    if (suflen > slen) return val_bool(0);
    return val_bool(strcmp(s + slen - suflen, suf) == 0);
}


/* ══════════════════════════════════════════════════════════
   STANDARD LIBRARIES
   Loaded via:  use math / use string / use list / use io / use os
══════════════════════════════════════════════════════════ */

/* ── math ─────────────────────────────────────────────── */
static Value *bi_tan  (Value **a, int n) { if(n<1)BUILTIN_ERR("tan: need num");   return val_num(tan  (a[0]->num)); }
static Value *bi_asin (Value **a, int n) { if(n<1)BUILTIN_ERR("asin: need num");  return val_num(asin (a[0]->num)); }
static Value *bi_acos (Value **a, int n) { if(n<1)BUILTIN_ERR("acos: need num");  return val_num(acos (a[0]->num)); }
static Value *bi_atan (Value **a, int n) { if(n<1)BUILTIN_ERR("atan: need num");  return val_num(atan (a[0]->num)); }
static Value *bi_atan2(Value **a, int n) { if(n<2)BUILTIN_ERR("atan2: need 2");   return val_num(atan2(a[0]->num, a[1]->num)); }
static Value *bi_exp  (Value **a, int n) { if(n<1)BUILTIN_ERR("exp: need num");   return val_num(exp  (a[0]->num)); }
static Value *bi_log2 (Value **a, int n) { if(n<1)BUILTIN_ERR("log2: need num");  return val_num(log2 (a[0]->num)); }
static Value *bi_log10(Value **a, int n) { if(n<1)BUILTIN_ERR("log10: need num"); return val_num(log10(a[0]->num)); }
static Value *bi_pow  (Value **a, int n) { if(n<2)BUILTIN_ERR("pow: need 2");     return val_num(pow  (a[0]->num, a[1]->num)); }
static Value *bi_hypot(Value **a, int n) { if(n<2)BUILTIN_ERR("hypot: need 2");   return val_num(hypot(a[0]->num, a[1]->num)); }
static Value *bi_trunc(Value **a, int n) { if(n<1)BUILTIN_ERR("trunc: need num"); return val_num(trunc(a[0]->num)); }
static Value *bi_sign (Value **a, int n) {
    if(n<1) BUILTIN_ERR("sign: need num");
    double v = a[0]->num;
    return val_num(v > 0 ? 1 : v < 0 ? -1 : 0);
}
static Value *bi_clamp(Value **a, int n) {
    if(n<3) BUILTIN_ERR("clamp: need (val, min, max)");
    double v=a[0]->num, lo=a[1]->num, hi=a[2]->num;
    return val_num(v < lo ? lo : v > hi ? hi : v);
}
static Value *bi_lerp(Value **a, int n) {
    if(n<3) BUILTIN_ERR("lerp: need (a, b, t)");
    return val_num(a[0]->num + (a[1]->num - a[0]->num) * a[2]->num);
}
static Value *bi_is_nan (Value **a, int n) { if(n<1)BUILTIN_ERR("is_nan: need num");  return val_bool(isnan (a[0]->num)); }
static Value *bi_is_inf (Value **a, int n) { if(n<1)BUILTIN_ERR("is_inf: need num");  return val_bool(isinf (a[0]->num)); }

/* ── rand ─────────────────────────────────────────────── */
#include <time.h>
static int rand_seeded = 0;
static Value *bi_rand(Value **args, int nargs) {
    if (!rand_seeded) { srand((unsigned)time(NULL)); rand_seeded = 1; }
    if (nargs == 0) return val_num((double)rand() / ((double)RAND_MAX + 1.0));
    if (nargs == 1) return val_num((double)(rand() % (int)args[0]->num));
    if (nargs >= 2) {
        int lo=(int)args[0]->num, hi=(int)args[1]->num;
        return val_num((double)(lo + rand() % (hi - lo)));
    }
    return val_nil();
}
static Value *bi_rand_seed(Value **args, int nargs) {
    if (nargs<1) BUILTIN_ERR("rand_seed: need num");
    srand((unsigned)args[0]->num); rand_seeded = 1;
    return val_nil();
}
static Value *bi_rand_choice(Value **args, int nargs) {
    if (!rand_seeded) { srand((unsigned)time(NULL)); rand_seeded = 1; }
    if (nargs<1||args[0]->type!=VT_LIST) BUILTIN_ERR("rand_choice: expected list");
    KiteList *l = args[0]->list;
    if (l->len == 0) return val_nil();
    return val_ref(l->items[rand() % l->len]);
}
static Value *bi_rand_shuffle(Value **args, int nargs) {
    if (!rand_seeded) { srand((unsigned)time(NULL)); rand_seeded = 1; }
    if (nargs<1||args[0]->type!=VT_LIST) BUILTIN_ERR("rand_shuffle: expected list");
    KiteList *l = args[0]->list;
    Value *res = val_list();
    for (int i=0;i<l->len;i++) list_push_val(res, l->items[i]);
    KiteList *rl = res->list;
    for (int i=rl->len-1;i>0;i--) {
        int j = rand() % (i+1);
        Value *tmp = rl->items[i]; rl->items[i]=rl->items[j]; rl->items[j]=tmp;
    }
    return res;
}

/* ── string ───────────────────────────────────────────── */
static Value *bi_str_repeat(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_STR||args[1]->type!=VT_NUM)
        BUILTIN_ERR("str_repeat: expected (str, n)");
    int n=(int)args[1]->num; const char *s=args[0]->str;
    size_t slen=strlen(s);
    char *buf=calloc(1, slen*(size_t)n+1);
    for (int i=0;i<n;i++) strcat(buf,s);
    Value *v=val_str(buf); free(buf); return v;
}
static Value *bi_str_pad_left(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("str_pad_left: expected (str, width[, char])");
    const char *s=args[0]->str;
    int width=(int)args[1]->num;
    char pad = (nargs>=3&&args[2]->type==VT_STR&&args[2]->str[0]) ? args[2]->str[0] : ' ';
    int slen=(int)strlen(s);
    if (slen>=width) return val_ref(args[0]);
    int plen=width-slen;
    char *buf=malloc((size_t)width+1);
    memset(buf, pad, (size_t)plen);
    memcpy(buf+plen, s, (size_t)slen+1);
    Value *v=val_str(buf); free(buf); return v;
}
static Value *bi_str_pad_right(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("str_pad_right: expected (str, width[, char])");
    const char *s=args[0]->str;
    int width=(int)args[1]->num;
    char pad = (nargs>=3&&args[2]->type==VT_STR&&args[2]->str[0]) ? args[2]->str[0] : ' ';
    int slen=(int)strlen(s);
    if (slen>=width) return val_ref(args[0]);
    char *buf=malloc((size_t)width+1);
    memcpy(buf, s, (size_t)slen);
    memset(buf+slen, pad, (size_t)(width-slen));
    buf[width]='\0';
    Value *v=val_str(buf); free(buf); return v;
}
static Value *bi_str_count(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_STR||args[1]->type!=VT_STR)
        BUILTIN_ERR("str_count: expected (str, sub)");
    const char *s=args[0]->str, *sub=args[1]->str;
    int sublen=(int)strlen(sub), count=0;
    if (sublen==0) return val_num((double)strlen(s)+1);
    for (const char *p=s; (p=strstr(p,sub)); p+=sublen) count++;
    return val_num((double)count);
}
static Value *bi_str_rev(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("str_rev: expected str");
    int n=(int)strlen(args[0]->str);
    char *buf=malloc((size_t)n+1);
    for (int i=0;i<n;i++) buf[i]=args[0]->str[n-1-i];
    buf[n]='\0'; Value *v=val_str(buf); free(buf); return v;
}
static Value *bi_str_is_num(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("str_is_num: expected str");
    char *end; strtod(args[0]->str, &end);
    return val_bool(*end=='\0' && end!=args[0]->str);
}
static Value *bi_str_lines(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("str_lines: expected str");
    Value *res=val_list();
    const char *s=args[0]->str;
    while (*s) {
        const char *nl=strchr(s,'\n');
        size_t len = nl ? (size_t)(nl-s) : strlen(s);
        char *line=malloc(len+1); memcpy(line,s,len); line[len]='\0';
        Value *v=val_str(line); list_push_val(res,v); val_unref(v); free(line);
        s = nl ? nl+1 : s+len;
    }
    return res;
}
static Value *bi_str_chars(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("str_chars: expected str");
    Value *res=val_list();
    char ch[2]={0,0};
    for (const char *p=args[0]->str; *p; p++) {
        ch[0]=*p; Value *v=val_str(ch); list_push_val(res,v); val_unref(v);
    }
    return res;
}

/* ── list ─────────────────────────────────────────────── */
static Value *bi_list_sum(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_LIST) BUILTIN_ERR("list_sum: expected list");
    double s=0; KiteList *l=args[0]->list;
    for (int i=0;i<l->len;i++) s+=l->items[i]->num;
    return val_num(s);
}
static Value *bi_list_avg(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_LIST) BUILTIN_ERR("list_avg: expected list");
    KiteList *l=args[0]->list; if(l->len==0) return val_nil();
    double s=0; for(int i=0;i<l->len;i++) s+=l->items[i]->num;
    return val_num(s/(double)l->len);
}
static Value *bi_list_uniq(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_LIST) BUILTIN_ERR("list_uniq: expected list");
    Value *res=val_list(); KiteList *l=args[0]->list;
    for (int i=0;i<l->len;i++) {
        int found=0;
        KiteList *rl=res->list;
        for (int j=0;j<rl->len;j++) if(val_eq(l->items[i],rl->items[j])){found=1;break;}
        if (!found) list_push_val(res,l->items[i]);
    }
    return res;
}
static Value *bi_list_flat(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_LIST) BUILTIN_ERR("list_flat: expected list");
    Value *res=val_list(); KiteList *l=args[0]->list;
    for (int i=0;i<l->len;i++) {
        if (l->items[i]->type==VT_LIST) {
            KiteList *inner=l->items[i]->list;
            for (int j=0;j<inner->len;j++) list_push_val(res,inner->items[j]);
        } else list_push_val(res,l->items[i]);
    }
    return res;
}
static Value *bi_list_zip(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST||args[1]->type!=VT_LIST)
        BUILTIN_ERR("list_zip: expected 2 lists");
    Value *res=val_list();
    KiteList *a=args[0]->list, *b=args[1]->list;
    int len = a->len < b->len ? a->len : b->len;
    for (int i=0;i<len;i++) {
        Value *pair=val_list();
        list_push_val(pair,a->items[i]);
        list_push_val(pair,b->items[i]);
        list_push_val(res,pair); val_unref(pair);
    }
    return res;
}
static Value *bi_list_count(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST) BUILTIN_ERR("list_count: expected (list, val)");
    KiteList *l=args[0]->list; int c=0;
    for (int i=0;i<l->len;i++) if(val_eq(l->items[i],args[1])) c++;
    return val_num((double)c);
}
static Value *bi_list_take(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST) BUILTIN_ERR("list_take: expected (list, n)");
    int n=(int)args[1]->num; KiteList *l=args[0]->list;
    if (n>l->len) n=l->len;
    Value *res=val_list();
    for (int i=0;i<n;i++) list_push_val(res,l->items[i]);
    return res;
}
static Value *bi_list_drop(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST) BUILTIN_ERR("list_drop: expected (list, n)");
    int n=(int)args[1]->num; KiteList *l=args[0]->list;
    Value *res=val_list();
    for (int i=n;i<l->len;i++) list_push_val(res,l->items[i]);
    return res;
}
static Value *bi_list_any(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST) BUILTIN_ERR("list_any: expected (list, fn)");
    KiteList *l=args[0]->list; Value *fn=args[1];
    for (int i=0;i<l->len;i++) {
        Value *r=call_user(fn,l->items[i],NULL);
        int t=val_truthy(r); val_unref(r);
        if (t) return val_bool(1);
    }
    return val_bool(0);
}
static Value *bi_list_all(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST) BUILTIN_ERR("list_all: expected (list, fn)");
    KiteList *l=args[0]->list; Value *fn=args[1];
    for (int i=0;i<l->len;i++) {
        Value *r=call_user(fn,l->items[i],NULL);
        int t=val_truthy(r); val_unref(r);
        if (!t) return val_bool(0);
    }
    return val_bool(1);
}
static Value *bi_list_find(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST) BUILTIN_ERR("list_find: expected (list, fn)");
    KiteList *l=args[0]->list; Value *fn=args[1];
    for (int i=0;i<l->len;i++) {
        Value *r=call_user(fn,l->items[i],NULL);
        int t=val_truthy(r); val_unref(r);
        if (t) return val_ref(l->items[i]);
    }
    return val_nil();
}
static Value *bi_list_group(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST) BUILTIN_ERR("list_group: expected (list, fn)");
    KiteList *l=args[0]->list; Value *fn=args[1];
    Value *res=val_map(); KiteMap *m=res->map;
    for (int i=0;i<l->len;i++) {
        Value *key=call_user(fn,l->items[i],NULL);
        char *ks=val_tostr(key); val_unref(key);
        int found=0;
        for (int j=0;j<m->len;j++) {
            if (strcmp(m->keys[j],ks)==0) {
                list_push_val(m->vals[j],l->items[i]); found=1; break;
            }
        }
        if (!found) {
            Value *bucket=val_list(); list_push_val(bucket,l->items[i]);
            if (m->len>=m->cap) { m->cap=m->cap?m->cap*2:8; m->keys=realloc(m->keys,(size_t)m->cap*sizeof(char*)); m->vals=realloc(m->vals,(size_t)m->cap*sizeof(Value*)); } (void)m->cap;
            m->keys[m->len]=strdup(ks); m->vals[m->len]=bucket; m->len++;
        }
        free(ks);
    }
    return res;
}

/* ── io ───────────────────────────────────────────────── */
static Value *bi_file_create(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("file_create: expected path");
    FILE *f = fopen(args[0]->str, "a");
    if (!f) { if(g_ip) kite_error(g_ip,0,"IOError","file_create: cannot create '%s'",args[0]->str); return val_bool(0); }
    fclose(f);
    return val_bool(1);
}

static Value *bi_file_read(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("file_read: expected path");
    FILE *f=fopen(args[0]->str,"r");
    if (!f) { if(g_ip) kite_error(g_ip,0,"IOError","file_read: cannot open '%s'",args[0]->str); return val_nil(); }
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *buf=malloc((size_t)sz+1);
    size_t got=fread(buf,1,(size_t)sz,f); buf[got]='\0'; fclose(f);
    Value *v=val_str(buf); free(buf); return v;
}
static Value *bi_file_write(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_STR||args[1]->type!=VT_STR)
        BUILTIN_ERR("file_write: expected (path, content)");
    FILE *f=fopen(args[0]->str,"w");
    if (!f) { if(g_ip) kite_error(g_ip,0,"IOError","file_write: cannot open '%s'",args[0]->str); return val_bool(0); }
    fputs(args[1]->str,f); fclose(f);
    return val_bool(1);
}
static Value *bi_file_append(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_STR||args[1]->type!=VT_STR)
        BUILTIN_ERR("file_append: expected (path, content)");
    FILE *f=fopen(args[0]->str,"a");
    if (!f) { if(g_ip) kite_error(g_ip,0,"IOError","file_append: cannot open '%s'",args[0]->str); return val_bool(0); }
    fputs(args[1]->str,f); fclose(f);
    return val_bool(1);
}
static Value *bi_file_lines(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("file_lines: expected path");
    FILE *f=fopen(args[0]->str,"r");
    if (!f) { if(g_ip) kite_error(g_ip,0,"IOError","file_lines: cannot open '%s'",args[0]->str); return val_nil(); }
    Value *res=val_list(); char line[4096];
    while (fgets(line,(int)sizeof(line),f)) {
        int n=(int)strlen(line);
        if (n>0&&line[n-1]=='\n') line[n-1]='\0';
        Value *v=val_str(line); list_push_val(res,v); val_unref(v);
    }
    fclose(f); return res;
}
static Value *bi_file_exists(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("file_exists: expected path");
    FILE *f=fopen(args[0]->str,"r");
    if (f) { fclose(f); return val_bool(1); }
    return val_bool(0);
}
static Value *bi_file_delete(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("file_delete: expected path");
    return val_bool(remove(args[0]->str)==0);
}
/* ── os ───────────────────────────────────────────────── */
#include <stdlib.h>
static Value *bi_os_env(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("os_env: expected var name");
    const char *v=getenv(args[0]->str);
    return v ? val_str(v) : val_nil();
}
static Value *bi_os_run(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("os_run: expected command");
    return val_num((double)system(args[0]->str));
}
static Value *bi_os_exit(Value **args, int nargs) {
    exit(nargs>=1 ? (int)args[0]->num : 0);
}
static Value *bi_os_time(Value **args, int nargs) {
    (void)args; (void)nargs;
    return val_num((double)time(NULL));
}
static Value *bi_os_clock(Value **args, int nargs) {
    (void)args; (void)nargs;
    return val_num((double)clock() / (double)CLOCKS_PER_SEC);
}
static Value *bi_os_shell(Value **args, int nargs) {
    /* run command and capture output */
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("os_shell: expected command");
    FILE *p=popen(args[0]->str,"r");
    if (!p) return val_nil();
    char *buf=NULL; size_t len=0; char tmp[256]; size_t got;
    while ((got=fread(tmp,1,sizeof(tmp),p))>0) {
        buf=realloc(buf,len+got+1);
        memcpy(buf+len,tmp,got); len+=got; buf[len]='\0';
    }
    pclose(p);
    if (!buf) return val_str("");
    /* strip trailing newline */
    if (len>0&&buf[len-1]=='\n') buf[--len]='\0';
    Value *v=val_str(buf); free(buf); return v;
}
static Value *bi_os_args(Value **args, int nargs) {
    (void)args; (void)nargs;
    /* Return empty list — args not stored globally in this impl */
    return val_list();
}

/* ── use: register stdlib into env ───────────────────── */
static void load_maps_stdlib(Env *env);   /* forward decl */
static void load_extra_stdlib(Env *env, const char *name); /* forward decl */
static void load_stdlib(Env *env, const char *name) {
    #define SREG(nm,fn) do { Value *v=val_builtin(fn,nm); env_def(env,nm,v); val_unref(v); } while(0)

    if (strcmp(name,"math")==0) {
        SREG("tan",       bi_tan);
        SREG("asin",      bi_asin);
        SREG("acos",      bi_acos);
        SREG("atan",      bi_atan);
        SREG("atan2",     bi_atan2);
        SREG("exp",       bi_exp);
        SREG("log2",      bi_log2);
        SREG("log10",     bi_log10);
        SREG("pow",       bi_pow);
        SREG("hypot",     bi_hypot);
        SREG("trunc",     bi_trunc);
        SREG("sign",      bi_sign);
        SREG("clamp",     bi_clamp);
        SREG("lerp",      bi_lerp);
        SREG("is_nan",    bi_is_nan);
        SREG("is_inf",    bi_is_inf);
        /* extra constants */
        Value *e=val_num(2.718281828459045); env_def(env,"E",e); val_unref(e);
        Value *inf=val_num(1.0/0.0); env_def(env,"INF",inf); val_unref(inf);
        return;
    }
    if (strcmp(name,"rand")==0) {
        SREG("rand",         bi_rand);
        SREG("rand_seed",    bi_rand_seed);
        SREG("rand_choice",  bi_rand_choice);
        SREG("rand_shuffle", bi_rand_shuffle);
        return;
    }
    if (strcmp(name,"string")==0) {
        SREG("str_repeat",   bi_str_repeat);
        SREG("str_pad_left", bi_str_pad_left);
        SREG("str_pad_right",bi_str_pad_right);
        SREG("str_count",    bi_str_count);
        SREG("str_rev",      bi_str_rev);
        SREG("str_is_num",   bi_str_is_num);
        SREG("str_lines",    bi_str_lines);
        SREG("str_chars",    bi_str_chars);
        return;
    }
    if (strcmp(name,"list")==0) {
        SREG("list_sum",   bi_list_sum);
        SREG("list_avg",   bi_list_avg);
        SREG("list_uniq",  bi_list_uniq);
        SREG("list_flat",  bi_list_flat);
        SREG("list_zip",   bi_list_zip);
        SREG("list_count", bi_list_count);
        SREG("list_take",  bi_list_take);
        SREG("list_drop",  bi_list_drop);
        SREG("list_any",   bi_list_any);
        SREG("list_all",   bi_list_all);
        SREG("list_find",  bi_list_find);
        SREG("list_group", bi_list_group);
        return;
    }
    if (strcmp(name,"io")==0) {
        SREG("file_create",  bi_file_create);
        SREG("file_read",   bi_file_read);
        SREG("file_write",  bi_file_write);
        SREG("file_append", bi_file_append);
        SREG("file_lines",  bi_file_lines);
        SREG("file_exists", bi_file_exists);
        SREG("file_delete", bi_file_delete);
        return;
    }
    if (strcmp(name,"maps")==0) { load_maps_stdlib(env); return; }
    if (strcmp(name,"os")==0) {
        SREG("os_env",   bi_os_env);
        SREG("os_run",   bi_os_run);
        SREG("os_exit",  bi_os_exit);
        SREG("os_time",  bi_os_time);
        SREG("os_clock", bi_os_clock);
        SREG("os_shell", bi_os_shell);
        SREG("os_args",  bi_os_args);
        return;
    }
    if (g_ip) kite_error(g_ip,0,"ImportError","use: unknown library '%s'", name);
    #undef SREG
}

/* ══════════════════════════════════════════════════════════
   MAPS STDLIB
══════════════════════════════════════════════════════════ */
static Value *bi_map_get(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_MAP||args[1]->type!=VT_STR)
        BUILTIN_ERR("map_get: expected (map, key [, default])");
    KiteMap *m = args[0]->map;
    for (int i=0;i<m->len;i++)
        if (strcmp(m->keys[i],args[1]->str)==0) return val_ref(m->vals[i]);
    return nargs>=3 ? val_ref(args[2]) : val_nil();
}

static Value *bi_map_set(Value **args, int nargs) {
    if (nargs<3||args[0]->type!=VT_MAP||args[1]->type!=VT_STR)
        BUILTIN_ERR("map_set: expected (map, key, val)");
    KiteMap *m = args[0]->map;
    for (int i=0;i<m->len;i++) {
        if (strcmp(m->keys[i],args[1]->str)==0) {
            val_unref(m->vals[i]); m->vals[i]=val_ref(args[2]); return val_ref(args[0]);
        }
    }
    if (m->len>=m->cap) {
        m->cap=m->cap?m->cap*2:8;
        m->keys=realloc(m->keys,(size_t)m->cap*sizeof(char*));
        m->vals=realloc(m->vals,(size_t)m->cap*sizeof(Value*));
    }
    m->keys[m->len]=strdup(args[1]->str);
    m->vals[m->len]=val_ref(args[2]);
    m->len++;
    return val_ref(args[0]);
}

static Value *bi_map_del(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_MAP||args[1]->type!=VT_STR)
        BUILTIN_ERR("map_del: expected (map, key)");
    KiteMap *m = args[0]->map;
    for (int i=0;i<m->len;i++) {
        if (strcmp(m->keys[i],args[1]->str)==0) {
            free(m->keys[i]); val_unref(m->vals[i]);
            for (int j=i;j<m->len-1;j++) {
                m->keys[j]=m->keys[j+1];
                m->vals[j]=m->vals[j+1];
            }
            m->len--;
            return val_bool(1);
        }
    }
    return val_bool(0);
}

static Value *bi_map_has(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_MAP||args[1]->type!=VT_STR)
        BUILTIN_ERR("map_has: expected (map, key)");
    KiteMap *m = args[0]->map;
    for (int i=0;i<m->len;i++)
        if (strcmp(m->keys[i],args[1]->str)==0) return val_bool(1);
    return val_bool(0);
}

static Value *bi_map_merge(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_MAP||args[1]->type!=VT_MAP)
        BUILTIN_ERR("map_merge: expected (map, map)");
    Value *res = val_map(); KiteMap *r=res->map;
    /* copy first map */
    KiteMap *a=args[0]->map;
    for (int i=0;i<a->len;i++) {
        if (r->len>=r->cap){r->cap=r->cap?r->cap*2:8;r->keys=realloc(r->keys,(size_t)r->cap*sizeof(char*));r->vals=realloc(r->vals,(size_t)r->cap*sizeof(Value*));}
        r->keys[r->len]=strdup(a->keys[i]); r->vals[r->len]=val_ref(a->vals[i]); r->len++;
    }
    /* merge second map — overwrite existing keys */
    KiteMap *b=args[1]->map;
    for (int i=0;i<b->len;i++) {
        int found=0;
        for (int j=0;j<r->len;j++) {
            if (strcmp(r->keys[j],b->keys[i])==0) {
                val_unref(r->vals[j]); r->vals[j]=val_ref(b->vals[i]); found=1; break;
            }
        }
        if (!found) {
            if (r->len>=r->cap){r->cap=r->cap?r->cap*2:8;r->keys=realloc(r->keys,(size_t)r->cap*sizeof(char*));r->vals=realloc(r->vals,(size_t)r->cap*sizeof(Value*));}
            r->keys[r->len]=strdup(b->keys[i]); r->vals[r->len]=val_ref(b->vals[i]); r->len++;
        }
    }
    return res;
}

static Value *bi_map_each(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_MAP) BUILTIN_ERR("map_each: expected (map, fn)");
    KiteMap *m=args[0]->map; Value *fn=args[1];
    for (int i=0;i<m->len;i++) {
        Value *k=val_str(m->keys[i]);
        Value *pair=val_list(); list_push_val(pair,k); list_push_val(pair,m->vals[i]); val_unref(k);
        Value *r=call_user(fn,pair,NULL); val_unref(pair); val_unref(r);
    }
    return val_nil();
}

static Value *bi_map_map(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_MAP) BUILTIN_ERR("map_map: expected (map, fn)");
    KiteMap *m=args[0]->map; Value *fn=args[1];
    Value *res=val_map(); KiteMap *r=res->map;
    for (int i=0;i<m->len;i++) {
        Value *k=val_str(m->keys[i]);
        Value *pair=val_list(); list_push_val(pair,k); list_push_val(pair,m->vals[i]); val_unref(k);
        Value *nv=call_user(fn,pair,NULL); if(!nv) nv=val_nil();
        if (r->len>=r->cap){r->cap=r->cap?r->cap*2:8;r->keys=realloc(r->keys,(size_t)r->cap*sizeof(char*));r->vals=realloc(r->vals,(size_t)r->cap*sizeof(Value*));}
        r->keys[r->len]=strdup(m->keys[i]); r->vals[r->len]=nv; r->len++;
    }
    return res;
}

static Value *bi_map_filter(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_MAP) BUILTIN_ERR("map_filter: expected (map, fn)");
    KiteMap *m=args[0]->map; Value *fn=args[1];
    Value *res=val_map(); KiteMap *r=res->map;
    for (int i=0;i<m->len;i++) {
        Value *k=val_str(m->keys[i]);
        Value *pair=val_list(); list_push_val(pair,k); list_push_val(pair,m->vals[i]); val_unref(k);
        Value *cond=call_user(fn,pair,NULL); val_unref(pair);
        int t=val_truthy(cond); val_unref(cond);
        if (t) {
            if (r->len>=r->cap){r->cap=r->cap?r->cap*2:8;r->keys=realloc(r->keys,(size_t)r->cap*sizeof(char*));r->vals=realloc(r->vals,(size_t)r->cap*sizeof(Value*));}
            r->keys[r->len]=strdup(m->keys[i]); r->vals[r->len]=val_ref(m->vals[i]); r->len++;
        }
    }
    return res;
}

static Value *bi_map_invert(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_MAP) BUILTIN_ERR("map_invert: expected map");
    KiteMap *m=args[0]->map;
    Value *res=val_map(); KiteMap *r=res->map;
    for (int i=0;i<m->len;i++) {
        char *newkey=val_tostr(m->vals[i]);
        if (r->len>=r->cap){r->cap=r->cap?r->cap*2:8;r->keys=realloc(r->keys,(size_t)r->cap*sizeof(char*));r->vals=realloc(r->vals,(size_t)r->cap*sizeof(Value*));}
        r->keys[r->len]=newkey; r->vals[r->len]=val_str(m->keys[i]); r->len++;
    }
    return res;
}

static Value *bi_map_from(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_LIST||args[1]->type!=VT_LIST)
        BUILTIN_ERR("map_from: expected (keys_list, vals_list)");
    KiteList *ks=args[0]->list, *vs=args[1]->list;
    int len=ks->len < vs->len ? ks->len : vs->len;
    Value *res=val_map(); KiteMap *r=res->map;
    for (int i=0;i<len;i++) {
        char *k=val_tostr(ks->items[i]);
        if (r->len>=r->cap){r->cap=r->cap?r->cap*2:8;r->keys=realloc(r->keys,(size_t)r->cap*sizeof(char*));r->vals=realloc(r->vals,(size_t)r->cap*sizeof(Value*));}
        r->keys[r->len]=k; r->vals[r->len]=val_ref(vs->items[i]); r->len++;
    }
    return res;
}

static Value *bi_map_to_list(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_MAP) BUILTIN_ERR("map_to_list: expected map");
    KiteMap *m=args[0]->map;
    Value *res=val_list();
    for (int i=0;i<m->len;i++) {
        Value *pair=val_list();
        Value *k=val_str(m->keys[i]);
        list_push_val(pair,k); list_push_val(pair,m->vals[i]); val_unref(k);
        list_push_val(res,pair); val_unref(pair);
    }
    return res;
}

static Value *bi_map_size(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_MAP) BUILTIN_ERR("map_size: expected map");
    return val_num((double)args[0]->map->len);
}

static Value *bi_map_keys_sorted(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_MAP) BUILTIN_ERR("map_keys_sorted: expected map");
    Value *res=val_list(); KiteMap *m=args[0]->map;
    for (int i=0;i<m->len;i++) { Value *k=val_str(m->keys[i]); list_push_val(res,k); val_unref(k); }
    /* sort alphabetically */
    KiteList *l=res->list;
    for (int i=1;i<l->len;i++) {
        Value *key=l->items[i]; int j=i-1;
        while (j>=0 && strcmp(l->items[j]->str,key->str)>0) { l->items[j+1]=l->items[j]; j--; }
        l->items[j+1]=key;
    }
    return res;
}

static void load_maps_stdlib(Env *env) {
    #define MREG(nm,fn) do { Value *v=val_builtin(fn,nm); env_def(env,nm,v); val_unref(v); } while(0)
    MREG("map_get",         bi_map_get);
    MREG("map_set",         bi_map_set);
    MREG("map_del",         bi_map_del);
    MREG("map_has",         bi_map_has);
    MREG("map_merge",       bi_map_merge);
    MREG("map_each",        bi_map_each);
    MREG("map_map",         bi_map_map);
    MREG("map_filter",      bi_map_filter);
    MREG("map_invert",      bi_map_invert);
    MREG("map_from",        bi_map_from);
    MREG("map_to_list",     bi_map_to_list);
    MREG("map_size",        bi_map_size);
    MREG("map_keys_sorted", bi_map_keys_sorted);
    #undef MREG
}

void setup_builtins(Env *env) {
    #define REG(nm,fn) do { Value *v=val_builtin(fn,nm); env_def(env,nm,v); val_unref(v); } while(0)
    REG("say",      bi_say);
    REG("talk",     bi_talk);
    REG("input",    bi_input);
    REG("type",     bi_type);
    REG("str",      bi_str_conv);
    REG("num",      bi_num_conv);
    REG("len",      bi_len);
    REG("push",     bi_push);
    REG("added",    bi_added);
    REG("pop",      bi_pop);
    REG("keys",     bi_keys);
    REG("vals",     bi_vals);
    REG("has",      bi_has);
    REG("range",    bi_range);
    REG("slice",    bi_slice);
    REG("reverse",  bi_reverse);
    REG("concat",   bi_concat);
    REG("sort",     bi_sort_fn);
    REG("map",      bi_map_fn);
    REG("filter",   bi_filter_fn);
    REG("reduce",   bi_reduce_fn);
    REG("floor",    bi_floor);
    REG("ceil",     bi_ceil);
    REG("round",    bi_round);
    REG("sqrt",     bi_sqrt);
    REG("abs",      bi_abs);
    REG("sin",      bi_sin);
    REG("cos",      bi_cos);
    REG("log",      bi_log);
    REG("min",      bi_min);
    REG("max",      bi_max);
    REG("split",    bi_split);
    REG("join",     bi_join);
    REG("upcase",   bi_upcase);
    REG("downcase", bi_downcase);
    REG("trim",     bi_trim);
    REG("index_of", bi_index_of);
    REG("replace",  bi_replace);
    REG("str_start",bi_str_start);
    REG("str_end",  bi_str_end);
    #undef REG
    Value *pi=val_num(3.14159265358979323846); env_def(env,"PI",pi);  val_unref(pi);
    Value *tau=val_num(6.28318530717958647692); env_def(env,"TAU",tau); val_unref(tau);
}

/* ══════════════════════════════════════════════════════════
   EVALUATOR
══════════════════════════════════════════════════════════ */
static Value *call_fn(Interp *ip, Value *fn, Value **args, int nargs, int line) {
    g_ip = ip;
    if (!fn) { kite_error(ip,line,"TypeError","nil is not callable"); return val_nil(); }
    if (fn->type==VT_BUILTIN) return fn->builtin.fn(args,nargs);
    if (fn->type==VT_FN) {
        KiteFn *kf=fn->fn;
        Env *fenv=env_new(kf->closure);
        for (int i=0;i<kf->nparams;i++) {
            Value *a = i<nargs ? val_ref(args[i]) : val_nil();
            env_def(fenv,kf->params[i],a); val_unref(a);
        }
        /* track current class for encapsulation.
           Use method_class (which class defines this fn) if known,
           otherwise fall back to instance's class. */
        KiteClass *prev_class = ip->current_class;
        KiteClass *method_owner = NULL;
        /* find which class owns this function by searching hierarchy */
        if (nargs > 0 && args[0] && args[0]->type==VT_INSTANCE) {
            KiteClass *kl = args[0]->instance->klass;
            while (kl) {
                Value *mv = env_get(kl->methods, kf->name ? kf->name : "");
                if (mv && mv->type==VT_FN && mv->fn==kf) { method_owner=kl; break; }
                kl = kl->parent;
            }
            if (!method_owner) method_owner = args[0]->instance->klass;
        }
        ip->current_class = method_owner;
        /* expose __super__ */
        if (method_owner && method_owner->parent) {
            Value *sv = val_str("__super_sentinel__");
            env_def(fenv, "__super__", sv); val_unref(sv);
        }
        Value *last=eval(ip,kf->body,fenv);
        env_unref(fenv);
        ip->current_class = prev_class;
        if (ip->has_return) {
            val_unref(last);
            Value *rv=ip->retval ? ip->retval : val_nil();
            ip->has_return=0; ip->retval=NULL;
            return rv;
        }
        return last; /* lambda implicit return */
    }
    kite_error(ip,line,"TypeError","not callable");
    return val_nil();
}

Value *eval(Interp *ip, Node *n, Env *env) {
    if (!n || ip->had_error) return val_nil();
    if (++ip->steps > MAX_STEPS) {
        kite_error(ip,n->line,"RuntimeError","execution limit reached (infinite loop?)");
        return val_nil();
    }

    switch (n->kind) {

    case ND_NIL:  return val_nil();
    case ND_BOOL: return val_bool(n->bval);
    case ND_NUM:  return val_num(n->num);
    case ND_STR:  return val_str(n->str);

    case ND_FMTSTR: {
        /* build string by evaluating each part and concatenating */
        char *result = strdup(""); size_t rlen=0;
        for (int i=0; i<n->fmtstr.len && !ip->had_error; i++) {
            Value *v  = eval(ip, n->fmtstr.items[i], env);
            char  *part = val_tostr(v); val_unref(v);
            size_t plen = strlen(part);
            result = realloc(result, rlen+plen+1);
            memcpy(result+rlen, part, plen+1);
            rlen += plen; free(part);
        }
        Value *res=val_str(result); free(result); return res;
    }

    case ND_IDENT: {
        Value *v=env_get(env,n->str);
        if (!v) { kite_error(ip,n->line,"NameError","undefined variable '%s'",n->str); return val_nil(); }
        return val_ref(v);
    }

    case ND_SET: {
        Value *v=eval(ip,n->set.val,env);
        env_def(env,n->set.name,v); val_unref(v);
        return val_nil();
    }

    case ND_ASSIGN: {
        Value *rhs=eval(ip,n->assign.val,env);
        const char *op=n->assign.op;
        Node *tgt=n->assign.target;

        if (tgt->kind==ND_IDENT) {
            Value *newval=rhs;
            if (strcmp(op,"=")!=0) {
                /* compound op — variable must exist */
                Value *cur=env_get(env,tgt->str);
                if (!cur) { kite_error(ip,n->line,"NameError","undefined '%s'",tgt->str); val_unref(rhs); return val_nil(); }
                if      (strcmp(op,"+=")==0) {
                    if (cur->type==VT_NUM) newval=val_num(cur->num+rhs->num);
                    else if (cur->type==VT_STR) {
                        char *s=malloc(strlen(cur->str)+strlen(rhs->str)+1);
                        strcpy(s,cur->str); strcat(s,rhs->str);
                        newval=val_str(s); free(s);
                    }
                }
                else if (strcmp(op,"-=")==0) newval=val_num(cur->num-rhs->num);
                else if (strcmp(op,"*=")==0) newval=val_num(cur->num*rhs->num);
                else if (strcmp(op,"/=")==0) {
                    if (rhs->num==0) { kite_error(ip,n->line,"ZeroDivisionError","division by zero"); val_unref(rhs); return val_nil(); }
                    newval=val_num(cur->num/rhs->num);
                }
                val_unref(rhs);
                /* save back */
                env_set(env, tgt->str, newval);
                Value *ret=val_ref(newval); val_unref(newval); return ret;
            }
            /* plain = : variable must already exist (use 'set' to declare) */
            if (!env_set(env,tgt->str,newval)) {
                kite_error(ip,n->line,"NameError",
                    "'%s' is not defined — use 'set %s = ...' to declare",
                    tgt->str, tgt->str);
                val_unref(newval);
                return val_nil();
            }
            Value *ret=val_ref(newval); val_unref(newval); return ret;
        }

        if (tgt->kind==ND_PROP) {
            Value *obj = eval(ip, tgt->prop.obj, env);
            if (obj->type==VT_INSTANCE) {
                /* encapsulation check for write */
                const char *wprop = tgt->prop.prop;
                if (wprop[0] == '_') {
                    int allowed = 0;
                    KiteClass *ok = obj->instance->klass;
                    while (ok && !allowed) {
                        if (ok == ip->current_class) { allowed = 1; break; }
                        ok = ok->parent;
                    }
                    if (!allowed) {
                        kite_error(ip,n->line,"AccessError",
                            "'%s' is private — cannot write from outside class", wprop);
                        val_unref(obj); val_unref(rhs); return val_nil();
                    }
                }
                /* handle compound operators: self.x += n  etc */
                Value *newval = rhs;
                if (strcmp(op,"=") != 0) {
                    Value *cur = env_get(obj->instance->fields, tgt->prop.prop);
                    if (!cur) {
                        KiteClass *kl2 = obj->instance->klass;
                        while (kl2 && !cur) { cur = env_get(kl2->methods, tgt->prop.prop); kl2 = kl2->parent; }
                    }
                    if (!cur) cur = val_nil();
                    if      (strcmp(op,"+=")==0) {
                        if (cur->type==VT_NUM) newval=val_num(cur->num+rhs->num);
                        else { char *s=malloc(strlen(val_tostr(cur))+strlen(val_tostr(rhs))+1);
                               strcpy(s,val_tostr(cur)); strcat(s,val_tostr(rhs)); newval=val_str(s); free(s); }
                    }
                    else if (strcmp(op,"-=")==0) newval=val_num(cur->num-rhs->num);
                    else if (strcmp(op,"*=")==0) newval=val_num(cur->num*rhs->num);
                    else if (strcmp(op,"/=")==0) {
                        if (rhs->num==0){kite_error(ip,n->line,"ZeroDivisionError","division by zero");val_unref(rhs);val_unref(obj);return val_nil();}
                        newval=val_num(cur->num/rhs->num);
                    }
                    val_unref(rhs);
                }
                if (!env_set(obj->instance->fields, tgt->prop.prop, newval))
                    env_def(obj->instance->fields, tgt->prop.prop, newval);
                if (newval != rhs) val_unref(newval);
            } else if (obj->type==VT_MAP) {
                KiteMap *m=obj->map; int found=0;
                for (int i=0;i<m->len;i++) {
                    if (strcmp(m->keys[i],tgt->prop.prop)==0){val_unref(m->vals[i]);m->vals[i]=val_ref(rhs);found=1;break;}
                }
                if (!found) {
                    if (m->len>=m->cap){m->cap=m->cap?m->cap*2:8;m->keys=realloc(m->keys,(size_t)m->cap*sizeof(char*));m->vals=realloc(m->vals,(size_t)m->cap*sizeof(Value*));}
                    m->keys[m->len]=strdup(tgt->prop.prop); m->vals[m->len]=val_ref(rhs); m->len++;
                }
                val_unref(rhs);
            }
            val_unref(obj);
            return val_nil();
        }
        if (tgt->kind==ND_INDEX) {
            Value *obj=eval(ip,tgt->idx.obj,env);
            Value *idx=eval(ip,tgt->idx.idx,env);
            if (obj->type==VT_LIST && idx->type==VT_NUM) {
                int ii=(int)idx->num;
                if (ii<0) ii=obj->list->len+ii;
                KiteList *l=obj->list;
                if (ii>=0 && ii<l->len) { val_unref(l->items[ii]); l->items[ii]=val_ref(rhs); }
                else if (ii==l->len) list_push_val(obj,rhs);
                else kite_error(ip,n->line,"IndexError","list index out of range");
            } else if (obj->type==VT_MAP) {
                char *key=val_tostr(idx); KiteMap *m=obj->map;
                int found=0;
                for (int i=0;i<m->len;i++) {
                    if (strcmp(m->keys[i],key)==0) { val_unref(m->vals[i]); m->vals[i]=val_ref(rhs); found=1; break; }
                }
                if (!found) {
                    if (m->len>=m->cap) { m->cap=m->cap?m->cap*2:8; m->keys=realloc(m->keys,(size_t)m->cap*sizeof(char*)); m->vals=realloc(m->vals,(size_t)m->cap*sizeof(Value*)); }
                    m->keys[m->len]=strdup(key); m->vals[m->len]=val_ref(rhs); m->len++;
                }
                free(key);
            }
            val_unref(obj); val_unref(idx); val_unref(rhs);
            return val_nil();
        }
        kite_error(ip,n->line,"ValueError","invalid assignment target");
        val_unref(rhs); return val_nil();
    }

    case ND_BINOP: {
        const char *op=n->binop.op;
        /* short-circuit */
        if (strcmp(op,"and")==0) {
            Value *l=eval(ip,n->binop.left,env);
            if (!val_truthy(l)) return l;
            val_unref(l); return eval(ip,n->binop.right,env);
        }
        if (strcmp(op,"or")==0) {
            Value *l=eval(ip,n->binop.left,env);
            if (val_truthy(l)) return l;
            val_unref(l); return eval(ip,n->binop.right,env);
        }
        Value *l=eval(ip,n->binop.left,env);
        Value *r=eval(ip,n->binop.right,env);
        Value *res=NULL;

        if      (strcmp(op,"+")==0) {
            if (l->type==VT_NUM && r->type==VT_NUM) res=val_num(l->num+r->num);
            else if (l->type==VT_STR || r->type==VT_STR) {
                char *ls=val_tostr(l), *rs=val_tostr(r);
                char *s=malloc(strlen(ls)+strlen(rs)+1); strcpy(s,ls); strcat(s,rs);
                res=val_str(s); free(s); free(ls); free(rs);
            } else if (l->type==VT_LIST && r->type==VT_LIST) {
                Value *tmp[2]={l,r}; res=bi_concat(tmp,2);
            } else kite_error(ip,n->line,"TypeError","'+' unsupported types");
        }
        else if (strcmp(op,"-")==0) {
            if (l->type==VT_NUM && r->type==VT_NUM) res=val_num(l->num-r->num);
            else kite_error(ip,n->line,"TypeError","'-' needs numbers");
        }
        else if (strcmp(op,"*")==0) {
            if (l->type==VT_NUM && r->type==VT_NUM) res=val_num(l->num*r->num);
            else if (l->type==VT_STR && r->type==VT_NUM) {
                int times=(int)r->num;
                size_t slen=strlen(l->str);
                char *s=calloc(1, slen*(size_t)times+1);
                for (int i=0;i<times;i++) strcat(s,l->str);
                res=val_str(s); free(s);
            } else kite_error(ip,n->line,"TypeError","'*' unsupported types");
        }
        else if (strcmp(op,"/")==0) {
            if (r->num==0.0) { kite_error(ip,n->line,"ZeroDivisionError","division by zero"); res=val_nil(); }
            else res=val_num(l->num/r->num);
        }
        else if (strcmp(op,"%")==0)  res=val_num(fmod(l->num,r->num));
        else if (strcmp(op,"^")==0)  res=val_num(pow(l->num,r->num));
        else if (strcmp(op,"..")==0) {
            char *ls=val_tostr(l), *rs=val_tostr(r);
            char *s=malloc(strlen(ls)+strlen(rs)+1); strcpy(s,ls); strcat(s,rs);
            res=val_str(s); free(s); free(ls); free(rs);
        }
        else if (strcmp(op,"==")==0) res=val_bool(val_eq(l,r));
        else if (strcmp(op,"!=")==0) res=val_bool(!val_eq(l,r));
        else if (strcmp(op,"<")==0) {
            if (l->type==VT_NUM && r->type==VT_NUM)   res=val_bool(l->num<r->num);
            else if (l->type==VT_STR && r->type==VT_STR) res=val_bool(strcmp(l->str,r->str)<0);
            else kite_error(ip,n->line,"TypeError","'<' needs comparable types");
        }
        else if (strcmp(op,">")==0) {
            if (l->type==VT_NUM && r->type==VT_NUM)   res=val_bool(l->num>r->num);
            else if (l->type==VT_STR && r->type==VT_STR) res=val_bool(strcmp(l->str,r->str)>0);
            else kite_error(ip,n->line,"TypeError","'>' needs comparable types");
        }
        else if (strcmp(op,"<=")==0) res=val_bool(l->num<=r->num);
        else if (strcmp(op,">=")==0) res=val_bool(l->num>=r->num);
        else if (strcmp(op,"is")==0) {
            /* check if l is instance of class r */
            if (l->type==VT_INSTANCE && r->type==VT_CLASS) {
                KiteClass *kl = l->instance->klass;
                int found = 0;
                while (kl) { if (kl==r->klass){found=1;break;} kl=kl->parent; }
                res = val_bool(found);
            } else res = val_bool(0);
        }
        else kite_error(ip,n->line,"RuntimeError","unknown operator '%s'",op);

        val_unref(l); val_unref(r);
        return res ? res : val_nil();
    }

    case ND_UNOP: {
        Value *v=eval(ip,n->unop.expr,env); Value *res=NULL;
        if (strcmp(n->unop.op,"-")==0) {
            if (v->type==VT_NUM) res=val_num(-v->num);
            else kite_error(ip,n->line,"TypeError","unary '-' needs number");
        } else res=val_bool(!val_truthy(v));
        val_unref(v); return res ? res : val_nil();
    }

    case ND_CALL: {
        g_ip=ip;
        /* Check if this is instance.method(...) call — bind self */
        Value *self_val = NULL;
        if (n->call.callee->kind == ND_PROP) {
            Value *obj = eval(ip, n->call.callee->prop.obj, env);
            if (!ip->had_error && (obj->type==VT_INSTANCE || obj->type==VT_CLASS)) {
                self_val = obj; /* keep ref */
            } else {
                val_unref(obj);
            }
        }
        Value *callee=eval(ip,n->call.callee,env);
        if (ip->had_error) { val_unref(callee); val_unref(self_val); return val_nil(); }
        /* Special: ClassName.new(...) */
        if (self_val && self_val->type==VT_CLASS &&
            n->call.callee->kind==ND_PROP &&
            strcmp(n->call.callee->prop.prop,"new")==0) {
            val_unref(callee);
            KiteClass *klass = self_val->klass;
            val_unref(self_val);
            /* create instance */
            KiteInstance *inst = calloc(1, sizeof(KiteInstance));
            inst->klass  = klass;
            inst->fields = env_new(NULL);
            inst->refs   = 0;
            /* copy default fields walking entire class hierarchy (parent first) */
            /* collect chain: grandparent -> parent -> klass */
            KiteClass *chain[64]; int chain_len = 0;
            for (KiteClass *kl2 = klass; kl2 && chain_len < 64; kl2 = kl2->parent)
                chain[chain_len++] = kl2;
            /* apply from root down so child overrides parent defaults */
            for (int ci = chain_len-1; ci >= 0; ci--) {
                for (int i=0;i<ENV_HASH;i++) {
                    EnvBucket *b = chain[ci]->methods->buckets[i];
                    while (b) {
                        if (b->val->type != VT_FN && b->val->type != VT_BUILTIN)
                            env_def(inst->fields, b->name, b->val);
                        b = b->next;
                    }
                }
            }
            Value *iv = val_instance(inst);
            /* call init if exists */
            Value *init_fn = NULL;
            KiteClass *kl = klass;
            while (kl && !init_fn) {
                Value *mv = env_get(kl->methods, "init");
                if (mv && mv->type==VT_FN) init_fn = val_ref(mv);
                kl = kl->parent;
            }
            if (init_fn) {
                int na = n->call.args.len;
                Value **args = malloc((size_t)(na+1)*sizeof(Value*));
                args[0] = val_ref(iv);
                for (int i=0;i<na;i++) args[i+1]=eval(ip,n->call.args.items[i],env);
                Value *r = call_fn(ip, init_fn, args, na+1, n->line);
                val_unref(r);
                for (int i=0;i<na+1;i++) val_unref(args[i]);
                free(args); val_unref(init_fn);
            }
            return iv;
        }
        /* prepend self for instance method calls */
        int prepend = (self_val && self_val->type==VT_INSTANCE && callee->type==VT_FN) ? 1 : 0;
        int na=n->call.args.len;
        int total = na + prepend;
        Value **args=malloc((size_t)(total ? total : 1)*sizeof(Value*));
        if (prepend) args[0] = val_ref(self_val);
        for (int i=0;i<na;i++) {
            args[i+prepend]=eval(ip,n->call.args.items[i],env);
            if (ip->had_error) {
                for (int j=0;j<i+prepend;j++) val_unref(args[j]);
                free(args); val_unref(callee); val_unref(self_val); return val_nil();
            }
        }
        na = total;
        Value *res=call_fn(ip,callee,args,na,n->line);
        for (int i=0;i<na;i++) val_unref(args[i]);
        free(args); val_unref(callee); val_unref(self_val);
        return res ? res : val_nil();
    }

    case ND_INDEX: {
        Value *obj=eval(ip,n->idx.obj,env);
        Value *idx=eval(ip,n->idx.idx,env);
        Value *res=NULL;
        if (obj->type==VT_LIST && idx->type==VT_NUM) {
            int i=(int)idx->num; if(i<0) i=obj->list->len+i;
            if (i>=0&&i<obj->list->len) res=val_ref(obj->list->items[i]);
            else { kite_error(ip,n->line,"IndexError","list index %d out of range",(int)idx->num); }
        } else if (obj->type==VT_STR && idx->type==VT_NUM) {
            int i=(int)idx->num; int ln=(int)strlen(obj->str); if(i<0) i=ln+i;
            if (i>=0&&i<ln) { char ch[2]={obj->str[i],'\0'}; res=val_str(ch); }
            else res=val_nil();
        } else if (obj->type==VT_MAP) {
            char *key=val_tostr(idx); KiteMap *m=obj->map;
            for (int i=0;i<m->len;i++) if(strcmp(m->keys[i],key)==0){res=val_ref(m->vals[i]);break;}
            free(key); if(!res) res=val_nil();
        } else { kite_error(ip,n->line,"TypeError","cannot index type '%s'",
                    obj->type==VT_NUM?"num":obj->type==VT_BOOL?"bool":"other"); }
        val_unref(obj); val_unref(idx);
        return res ? res : val_nil();
    }

    case ND_PROP: {
        Value *obj=eval(ip,n->prop.obj,env); Value *res=NULL;
        if (obj->type==VT_MAP) {
            KiteMap *m=obj->map;
            for (int i=0;i<m->len;i++) if(strcmp(m->keys[i],n->prop.prop)==0){res=val_ref(m->vals[i]);break;}
        } else if (obj->type==VT_INSTANCE) {
            const char *prop = n->prop.prop;
            /* encapsulation: _ prefix = private, only accessible from same class */
            if (prop[0] == '_') {
                int allowed = 0;
                if (ip->current_class) {
                    KiteClass *kl2 = ip->current_class;
                    while (kl2) {
                        if (kl2 == obj->instance->klass ||
                            /* child class can access parent private too */
                            obj->instance->klass == kl2) { allowed = 1; break; }
                        kl2 = kl2->parent;
                    }
                    /* check if obj's class is in current_class hierarchy */
                    KiteClass *ok = obj->instance->klass;
                    while (ok && !allowed) {
                        if (ok == ip->current_class) { allowed = 1; break; }
                        ok = ok->parent;
                    }
                }
                if (!allowed) {
                    kite_error(ip,n->line,"AccessError",
                        "'%s' is private — cannot access from outside class", prop);
                    val_unref(obj); return val_nil();
                }
            }
            /* look in fields first, then methods (class hierarchy) */
            Value *fv = env_get(obj->instance->fields, prop);
            if (fv) { res = val_ref(fv); }
            else {
                KiteClass *kl = obj->instance->klass;
                while (kl && !res) {
                    Value *mv = env_get(kl->methods, prop);
                    if (mv) res = val_ref(mv);
                    kl = kl->parent;
                }
            }
        } else if (obj->type==VT_CLASS) {
            /* Class.new() or static method */
            Value *mv = env_get(obj->klass->methods, n->prop.prop);
            if (mv) res = val_ref(mv);
        } else if (obj->type==VT_STR && strcmp(obj->str,"__super_sentinel__")==0) {
            /* super.method — look up from parent of current_class */
            if (ip->current_class && ip->current_class->parent) {
                KiteClass *kl = ip->current_class->parent;
                while (kl && !res) {
                    Value *mv = env_get(kl->methods, n->prop.prop);
                    if (mv) res = val_ref(mv);
                    kl = kl->parent;
                }
            }
        }
        val_unref(obj);
        return res ? res : val_nil();
    }

    case ND_LIST: {
        Value *res=val_list();
        for (int i=0;i<n->list.len;i++) {
            Value *item=eval(ip,n->list.items[i],env);
            list_push_val(res,item); val_unref(item);
        }
        return res;
    }

    case ND_MAP: {
        Value *res=val_map(); KiteMap *m=res->map;
        for (int i=0;i<n->map.len;i++) {
            Value *v=eval(ip,n->map.pairs[i].val,env);
            if (m->len>=m->cap) { m->cap=m->cap?m->cap*2:8; m->keys=realloc(m->keys,(size_t)m->cap*sizeof(char*)); m->vals=realloc(m->vals,(size_t)m->cap*sizeof(Value*)); }
            m->keys[m->len]=strdup(n->map.pairs[i].key); m->vals[m->len]=v; m->len++;
        }
        return res;
    }

    case ND_LAMBDA: {
        KiteFn *kf=calloc(1,sizeof(KiteFn));
        kf->nparams=n->lambda.nparams;
        kf->params=malloc((size_t)(kf->nparams ? kf->nparams : 1)*sizeof(char*));
        for (int i=0;i<kf->nparams;i++) kf->params[i]=strdup(n->lambda.params[i]);
        kf->body=n->lambda.body; kf->closure=env; env_ref(env); kf->refs=0;
        return val_fn(kf);
    }

    case ND_DEF: {
        KiteFn *kf=calloc(1,sizeof(KiteFn));
        kf->name=strdup(n->def.name); kf->nparams=n->def.nparams;
        kf->params=malloc((size_t)(kf->nparams ? kf->nparams : 1)*sizeof(char*));
        for (int i=0;i<kf->nparams;i++) kf->params[i]=strdup(n->def.params[i]);
        kf->body=n->def.body; kf->closure=env; env_ref(env); kf->refs=0;
        Value *v=val_fn(kf); env_def(env,n->def.name,v); val_unref(v);
        return val_nil();
    }

    case ND_WHEN: {
        for (int i=0;i<n->when.nbranch;i++) {
            int taken=0;
            if (!n->when.conds[i]) { taken=1; }
            else { Value *cv=eval(ip,n->when.conds[i],env); taken=val_truthy(cv); val_unref(cv); }
            if (taken) {
                Env *be=env_new(env);
                eval_block(ip,&n->when.bodies[i]->block,be);
                env_unref(be); return val_nil();
            }
        }
        return val_nil();
    }

    case ND_LOOP_WHILE: {
        long long guard=0;
        while (1) {
            Value *cond=eval(ip,n->loop_while.cond,env);
            int t=val_truthy(cond); val_unref(cond);
            if (!t||ip->had_error) break;
            if (++guard>2000000) { kite_error(ip,n->line,"RuntimeError","loop limit reached"); break; }
            Env *le=env_new(env); eval_block(ip,&n->loop_while.body->block,le); env_unref(le);
            if (ip->has_return) break;
            if (ip->has_break) { ip->has_break=0; break; }
            if (ip->has_next)  { ip->has_next=0;  continue; }
        }
        return val_nil();
    }

    case ND_LOOP_FOR: {
        Value *iter=eval(ip,n->loop_for.iter,env);
        if (iter->type==VT_LIST) {
            KiteList *l=iter->list;
            for (int i=0;i<l->len&&!ip->had_error;i++) {
                Env *le=env_new(env); env_def(le,n->loop_for.var,l->items[i]);
                eval_block(ip,&n->loop_for.body->block,le); env_unref(le);
                if (ip->has_return) break;
                if (ip->has_break) { ip->has_break=0; break; }
                if (ip->has_next)  { ip->has_next=0;  continue; }
            }
        } else if (iter->type==VT_STR) {
            const char *s=iter->str;
            for (int i=0;s[i]&&!ip->had_error;i++) {
                char ch[2]={s[i],'\0'}; Value *cv=val_str(ch);
                Env *le=env_new(env); env_def(le,n->loop_for.var,cv); val_unref(cv);
                eval_block(ip,&n->loop_for.body->block,le); env_unref(le);
                if (ip->has_return||ip->has_break) { ip->has_break=0; break; }
                if (ip->has_next) { ip->has_next=0; continue; }
            }
        } else kite_error(ip,n->line,"TypeError","for: can only iterate list or str");
        val_unref(iter); return val_nil();
    }

    case ND_GIVE: {
        Value *v=n->child ? eval(ip,n->child,env) : val_nil();
        ip->has_return=1;
        if (ip->retval) val_unref(ip->retval);
        ip->retval=v; return val_nil();
    }


    case ND_USE: {
        g_ip = ip;
        /* Try stdlib first */
        const char *stdlib_names[] = {
            "math","rand","string","list","io","os","maps","decimal","regex","time", NULL
        };
        int is_stdlib = 0;
        for (int si=0; stdlib_names[si]; si++)
            if (strcmp(n->str, stdlib_names[si])==0) { is_stdlib=1; break; }

        if (is_stdlib) {
            if (strcmp(n->str,"decimal")==0 || strcmp(n->str,"regex")==0 || strcmp(n->str,"time")==0)
                load_extra_stdlib(env, n->str);
            else
                load_stdlib(env, n->str);
        } else {
            /* Try to load as user module: look for name.kite in script_dir */
            char path[600];
            snprintf(path, sizeof(path), "%s/%s.kite",
                ip->script_dir[0] ? ip->script_dir : ".", n->str);

            FILE *f = fopen(path, "r");
            /* If not found, try current dir */
            if (!f) {
                snprintf(path, sizeof(path), "./%s.kite", n->str);
                f = fopen(path, "r");
            }
            if (!f) {
                kite_error(ip, n->line, "ImportError",
                    "module '%s' not found (tried %s/%s.kite)",
                    n->str, ip->script_dir[0] ? ip->script_dir : ".", n->str);
                return val_nil();
            }
            fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
            char *msrc=malloc((size_t)sz+1);
            size_t got=fread(msrc,1,(size_t)sz,f); msrc[got]='\0'; fclose(f);

            /* Parse and eval the module in the same env */
            Lexer *ml = lexer_new(msrc); lexer_tokenize(ml);
            Node  *mast = parse(ml);
            if (mast) {
                eval(ip, mast, env);
                /* NOTE: do NOT free mast — function bodies reference its nodes */
            }
            lexer_free(ml); free(msrc);
        }
        return val_nil();
    }



    case ND_OBJ: {
        /* Create a class and register it in env */
        KiteClass *klass = calloc(1, sizeof(KiteClass));
        klass->name   = strdup(n->obj_def.name);
        klass->parent = NULL;
        klass->refs   = 0;

        /* Resolve parent class */
        if (n->obj_def.parent) {
            Value *pv = env_get(env, n->obj_def.parent);
            if (pv && pv->type == VT_CLASS) {
                klass->parent = pv->klass;
            } else {
                kite_error(ip,n->line,"NameError","unknown parent class '%s'",n->obj_def.parent);
                free(klass->name); free(klass);
                return val_nil();
            }
        }

        /* Build method env — evaluate def statements */
        klass->methods = env_new(NULL);
        for (int i = 0; i < n->obj_def.body.len; i++) {
            Node *stmt = n->obj_def.body.items[i];
            if (stmt->kind == ND_DEF) {
                /* create fn bound to no closure (self passed explicitly) */
                KiteFn *kf = calloc(1, sizeof(KiteFn));
                kf->name    = strdup(stmt->def.name);
                kf->nparams = stmt->def.nparams;
                kf->params  = malloc((size_t)(kf->nparams?kf->nparams:1)*sizeof(char*));
                for (int j=0;j<kf->nparams;j++) kf->params[j]=strdup(stmt->def.params[j]);
                kf->body    = stmt->def.body;
                kf->closure = env;  /* capture current env for closures */
                env_ref(env);
                kf->refs    = 0;
                Value *fv = val_fn(kf);
                env_def(klass->methods, stmt->def.name, fv);
                val_unref(fv);
            } else if (stmt->kind == ND_SET) {
                /* default field value — store as special __field__ */
                Value *dv = eval(ip, stmt->set.val, env);
                env_def(klass->methods, stmt->set.name, dv);
                val_unref(dv);
            } else if (stmt->kind == ND_EXPR_STMT) {
                /* ignore */
            }
        }

        Value *cv = val_class(klass);
        env_def(env, n->obj_def.name, cv);
        val_unref(cv);
        return val_nil();
    }

    case ND_DO: {
        /* run body, catch errors */
        Env *benv = env_new(env);
        eval_block(ip, &n->do_catch.body->block, benv);
        env_unref(benv);

        if (ip->had_error) {
            /* find matching handler */
            char caught_type[64], caught_msg[512];
            snprintf(caught_type, sizeof(caught_type), "%s", ip->errtype);
            snprintf(caught_msg,  sizeof(caught_msg),  "%s", ip->errbuf);

            int handled = 0;
            for (int i = 0; i < n->do_catch.nhandlers; i++) {
                ErrHandler *h = &n->do_catch.handlers[i];
                /* match if no type specified, or type matches */
                if (h->err_type == NULL ||
                    strcmp(h->err_type, caught_type) == 0) {
                    /* clear error state */
                    ip->had_error  = 0;
                    ip->errtype[0] = '\0';
                    ip->errbuf[0]  = '\0';
                    /* expose err_msg and err_type in handler scope */
                    Env *henv = env_new(env);
                    /* strip "line N: " prefix for clean message */
                    const char *clean_msg = caught_msg;
                    if (strncmp(clean_msg,"line ",5)==0) {
                        const char *colon = strchr(clean_msg+5,':');
                        if (colon) clean_msg = colon+2;
                    }
                    Value *vmsg  = val_str(clean_msg);
                    Value *vtype = val_str(caught_type);
                    env_def(henv, "err_msg",  vmsg);
                    env_def(henv, "err_type", vtype);
                    val_unref(vmsg); val_unref(vtype);
                    eval_block(ip, &h->body->block, henv);
                    env_unref(henv);
                    handled = 1;
                    break;
                }
            }
            /* if no handler matched, error propagates */
            (void)handled;
        }
        return val_nil();
    }

    case ND_BREAK: ip->has_break=1; return val_nil();
    case ND_NEXT:  ip->has_next=1;  return val_nil();

    case ND_EXPR_STMT: {
        Value *v=eval(ip,n->child,env); val_unref(v); return val_nil();
    }

    case ND_BLOCK:
    case ND_PROGRAM:
        return eval_block(ip,&n->block,env);

    default:
        kite_error(ip,n->line,"RuntimeError","unknown node kind %d",n->kind);
        return val_nil();
    }
}

/* ══════════════════════════════════════════════════════════
   DECIMAL  —  точная арифметика через строки (без плавучих ошибок)
   Хранит число как строку, операции через long double
══════════════════════════════════════════════════════════ */
#include <locale.h>

static char *ld_to_str(long double v, int prec) {
    char buf[128];
    if (v == (long long)v && prec == 0)
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
    else
        snprintf(buf, sizeof(buf), "%.*Lf", prec, v);
    /* strip trailing zeros after dot */
    if (strchr(buf, '.')) {
        int n = (int)strlen(buf)-1;
        while (n>0 && buf[n]=='0') buf[n--]='\0';
        if (buf[n]=='.') buf[n]='\0';
    }
    return strdup(buf);
}

static Value *bi_dec(Value **args, int nargs) {
    if (nargs<1) BUILTIN_ERR("dec: expected value");
    char *s = val_tostr(args[0]); Value *v = val_str(s); free(s); return v;
}
static Value *bi_dec_add(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("dec_add: expected 2 args");
    long double a=strtold(args[0]->str,NULL), b=strtold(args[1]->str,NULL);
    char *s=ld_to_str(a+b,20); Value *v=val_str(s); free(s); return v;
}
static Value *bi_dec_sub(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("dec_sub: expected 2 args");
    long double a=strtold(args[0]->str,NULL), b=strtold(args[1]->str,NULL);
    char *s=ld_to_str(a-b,20); Value *v=val_str(s); free(s); return v;
}
static Value *bi_dec_mul(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("dec_mul: expected 2 args");
    long double a=strtold(args[0]->str,NULL), b=strtold(args[1]->str,NULL);
    char *s=ld_to_str(a*b,20); Value *v=val_str(s); free(s); return v;
}
static Value *bi_dec_div(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("dec_div: expected 2 args");
    long double a=strtold(args[0]->str,NULL), b=strtold(args[1]->str,NULL);
    if (b==0.0L) BUILTIN_ERR("dec_div: division by zero");
    char *s=ld_to_str(a/b,20); Value *v=val_str(s); free(s); return v;
}
static Value *bi_dec_eq(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("dec_eq: expected 2 args");
    long double a=strtold(args[0]->str,NULL), b=strtold(args[1]->str,NULL);
    return val_bool(a==b);
}
static Value *bi_dec_lt(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("dec_lt: expected 2 args");
    return val_bool(strtold(args[0]->str,NULL) < strtold(args[1]->str,NULL));
}
static Value *bi_dec_gt(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("dec_gt: expected 2 args");
    return val_bool(strtold(args[0]->str,NULL) > strtold(args[1]->str,NULL));
}
static Value *bi_dec_round(Value **args, int nargs) {
    if (nargs<1) BUILTIN_ERR("dec_round: expected (dec [, places])");
    long double v=strtold(args[0]->str,NULL);
    int places = nargs>=2 ? (int)args[1]->num : 0;
    long double factor=1.0L; for(int i=0;i<places;i++) factor*=10.0L;
    long double rounded=roundl(v*factor)/factor;
    char *s=ld_to_str(rounded,places); Value *r=val_str(s); free(s); return r;
}
static Value *bi_dec_to_num(Value **args, int nargs) {
    if (nargs<1) BUILTIN_ERR("dec_to_num: expected dec");
    return val_num((double)strtold(args[0]->str,NULL));
}
static Value *bi_dec_format(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("dec_format: expected (dec, places)");
    long double v=strtold(args[0]->str,NULL);
    int places=(int)args[1]->num;
    char buf[128]; snprintf(buf,sizeof(buf),"%.*Lf",places,v);
    return val_str(buf);
}

/* ══════════════════════════════════════════════════════════
   REGEX  —  POSIX регулярные выражения
══════════════════════════════════════════════════════════ */
#include <regex.h>

static Value *bi_regex_match(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_STR||args[1]->type!=VT_STR)
        BUILTIN_ERR("regex_match: expected (str, pattern)");
    regex_t re; int r=regcomp(&re,args[1]->str,REG_EXTENDED|REG_NOSUB);
    if (r!=0) BUILTIN_ERR("regex_match: invalid pattern");
    int match=regexec(&re,args[0]->str,0,NULL,0)==0;
    regfree(&re); return val_bool(match);
}
static Value *bi_regex_find(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_STR||args[1]->type!=VT_STR)
        BUILTIN_ERR("regex_find: expected (str, pattern)");
    regex_t re; int r=regcomp(&re,args[1]->str,REG_EXTENDED);
    if (r!=0) BUILTIN_ERR("regex_find: invalid pattern");
    regmatch_t m;
    Value *res=val_list();
    const char *s=args[0]->str;
    int offset=0;
    while (regexec(&re,s+offset,1,&m,0)==0) {
        int start=offset+m.rm_so, end=offset+m.rm_eo;
        char *buf=malloc((size_t)(end-start)+1);
        memcpy(buf,args[0]->str+start,(size_t)(end-start)); buf[end-start]='\0';
        Value *v=val_str(buf); list_push_val(res,v); val_unref(v); free(buf);
        offset=end; if (m.rm_so==m.rm_eo) offset++;
    }
    regfree(&re); return res;
}
static Value *bi_regex_replace(Value **args, int nargs) {
    if (nargs<3||args[0]->type!=VT_STR||args[1]->type!=VT_STR||args[2]->type!=VT_STR)
        BUILTIN_ERR("regex_replace: expected (str, pattern, replacement)");
    regex_t re; int r=regcomp(&re,args[1]->str,REG_EXTENDED);
    if (r!=0) BUILTIN_ERR("regex_replace: invalid pattern");
    const char *s=args[0]->str, *repl=args[2]->str;
    char *result=strdup(""); size_t rlen=0;
    regmatch_t m; int offset=0;
    while (regexec(&re,s+offset,1,&m,0)==0) {
        /* append part before match */
        size_t pre=(size_t)m.rm_so;
        result=realloc(result,rlen+pre+strlen(repl)+1);
        memcpy(result+rlen,s+offset,pre); rlen+=pre;
        memcpy(result+rlen,repl,strlen(repl)); rlen+=strlen(repl);
        result[rlen]='\0';
        offset+=m.rm_eo; if (m.rm_so==m.rm_eo) { if (s[offset]) { result=realloc(result,rlen+2); result[rlen++]=s[offset++]; result[rlen]='\0'; } else break; }
    }
    /* append remainder */
    result=realloc(result,rlen+strlen(s+offset)+1);
    strcat(result,s+offset);
    regfree(&re); Value *v=val_str(result); free(result); return v;
}
static Value *bi_regex_split(Value **args, int nargs) {
    if (nargs<2||args[0]->type!=VT_STR||args[1]->type!=VT_STR)
        BUILTIN_ERR("regex_split: expected (str, pattern)");
    regex_t re; int r=regcomp(&re,args[1]->str,REG_EXTENDED);
    if (r!=0) BUILTIN_ERR("regex_split: invalid pattern");
    Value *res=val_list();
    const char *s=args[0]->str; int offset=0; regmatch_t m;
    while (regexec(&re,s+offset,1,&m,0)==0) {
        char *buf=malloc((size_t)m.rm_so+1);
        memcpy(buf,s+offset,(size_t)m.rm_so); buf[m.rm_so]='\0';
        Value *v=val_str(buf); list_push_val(res,v); val_unref(v); free(buf);
        offset+=m.rm_eo; if (m.rm_so==m.rm_eo) offset++;
    }
    Value *tail=val_str(s+offset); list_push_val(res,tail); val_unref(tail);
    regfree(&re); return res;
}

/* ══════════════════════════════════════════════════════════
   TIME  —  дата и время
══════════════════════════════════════════════════════════ */
#include <time.h>
#ifndef strptime
extern char *strptime(const char*, const char*, struct tm*);
#endif

static Value *bi_time_now(Value **args, int nargs) {
    (void)args; (void)nargs;
    return val_num((double)time(NULL));
}
static Value *bi_time_format(Value **args, int nargs) {
    if (nargs<1) BUILTIN_ERR("time_format: expected (timestamp [, fmt])");
    time_t ts=(time_t)args[0]->num;
    const char *fmt = nargs>=2&&args[1]->type==VT_STR ? args[1]->str : "%Y-%m-%d %H:%M:%S";
    struct tm *t=localtime(&ts);
    char buf[256]; strftime(buf,sizeof(buf),fmt,t);
    return val_str(buf);
}
static Value *bi_time_parse(Value **args, int nargs) {
    if (nargs<1||args[0]->type!=VT_STR) BUILTIN_ERR("time_parse: expected str");
    const char *fmt = nargs>=2&&args[1]->type==VT_STR ? args[1]->str : "%Y-%m-%d %H:%M:%S";
    struct tm t={0}; strptime(args[0]->str,fmt,&t);
    return val_num((double)mktime(&t));
}
static Value *bi_time_parts(Value **args, int nargs) {
    if (nargs<1) BUILTIN_ERR("time_parts: expected timestamp");
    time_t ts=(time_t)args[0]->num;
    struct tm *t=localtime(&ts);
    Value *m=val_map(); KiteMap *km=m->map;
    #define TSET(k,v) do { \
        if(km->len>=km->cap){km->cap=km->cap?km->cap*2:16; \
        km->keys=realloc(km->keys,(size_t)km->cap*sizeof(char*)); \
        km->vals=realloc(km->vals,(size_t)km->cap*sizeof(Value*));} \
        km->keys[km->len]=strdup(k); km->vals[km->len]=val_num((double)(v)); km->len++; } while(0)
    TSET("year",  t->tm_year+1900);
    TSET("month", t->tm_mon+1);
    TSET("day",   t->tm_mday);
    TSET("hour",  t->tm_hour);
    TSET("min",   t->tm_min);
    TSET("sec",   t->tm_sec);
    TSET("wday",  t->tm_wday);  /* 0=вс, 1=пн ... */
    TSET("yday",  t->tm_yday+1);
    #undef TSET
    return m;
}
static Value *bi_time_diff(Value **args, int nargs) {
    if (nargs<2) BUILTIN_ERR("time_diff: expected (ts1, ts2)");
    return val_num(fabs(args[0]->num - args[1]->num));
}
static Value *bi_time_add(Value **args, int nargs) {
    if (nargs<3) BUILTIN_ERR("time_add: expected (ts, amount, unit)");
    double ts=args[0]->num, n=args[1]->num;
    const char *unit = args[2]->type==VT_STR ? args[2]->str : "sec";
    double secs=n;
    if      (strcmp(unit,"min")==0)   secs=n*60;
    else if (strcmp(unit,"hour")==0)  secs=n*3600;
    else if (strcmp(unit,"day")==0)   secs=n*86400;
    else if (strcmp(unit,"week")==0)  secs=n*604800;
    else if (strcmp(unit,"month")==0) secs=n*2592000;
    else if (strcmp(unit,"year")==0)  secs=n*31536000;
    return val_num(ts+secs);
}
static Value *bi_time_weekday(Value **args, int nargs) {
    if (nargs<1) BUILTIN_ERR("time_weekday: expected timestamp");
    time_t ts=(time_t)args[0]->num;
    struct tm *t=localtime(&ts);
    const char *days[]={"Воскресенье","Понедельник","Вторник","Среда","Четверг","Пятница","Суббота"};
    return val_str(days[t->tm_wday]);
}
static Value *bi_time_sleep(Value **args, int nargs) {
    if (nargs<1) BUILTIN_ERR("time_sleep: expected seconds");
    struct timespec ts;
    ts.tv_sec  = (time_t)args[0]->num;
    ts.tv_nsec = (long)((args[0]->num - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
    return val_nil();
}

/* ── Register new libs in load_stdlib ── */
static void load_extra_stdlib(Env *env, const char *name) {
    #define EREG(nm,fn) do { Value *v=val_builtin(fn,nm); env_def(env,nm,v); val_unref(v); } while(0)
    if (strcmp(name,"decimal")==0) {
        EREG("dec",         bi_dec);
        EREG("dec_add",     bi_dec_add);
        EREG("dec_sub",     bi_dec_sub);
        EREG("dec_mul",     bi_dec_mul);
        EREG("dec_div",     bi_dec_div);
        EREG("dec_eq",      bi_dec_eq);
        EREG("dec_lt",      bi_dec_lt);
        EREG("dec_gt",      bi_dec_gt);
        EREG("dec_round",   bi_dec_round);
        EREG("dec_to_num",  bi_dec_to_num);
        EREG("dec_format",  bi_dec_format);
        return;
    }
    if (strcmp(name,"regex")==0) {
        EREG("regex_match",   bi_regex_match);
        EREG("regex_find",    bi_regex_find);
        EREG("regex_replace", bi_regex_replace);
        EREG("regex_split",   bi_regex_split);
        return;
    }
    if (strcmp(name,"time")==0) {
        EREG("time_now",     bi_time_now);
        EREG("time_format",  bi_time_format);
        EREG("time_parse",   bi_time_parse);
        EREG("time_parts",   bi_time_parts);
        EREG("time_diff",    bi_time_diff);
        EREG("time_add",     bi_time_add);
        EREG("time_weekday", bi_time_weekday);
        EREG("time_sleep",   bi_time_sleep);
        return;
    }
    #undef EREG
}
