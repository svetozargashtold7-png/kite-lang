#include "kite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "kite: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static char *read_stdin(void) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    return buf;
}

static int run_source(const char *src, int verbose) {
    Lexer *l = lexer_new(src);
    lexer_tokenize(l);
    if (verbose) {
        printf("=== Tokens (%d) ===\n", l->buf_len);
        for (int i = 0; i < l->buf_len; i++) {
            Token *t = &l->buf[i];
            printf("  [%2d] kind=%-3d str=%-16s num=%g\n",
                t->line, t->kind, t->str ? t->str : "-", t->num);
        }
    }
    Node *ast = parse(l);
    if (!ast) { lexer_free(l); return 1; }
    Env *env = env_new(NULL);
    setup_builtins(env);
    Interp *ip = interp_new();
    eval(ip, ast, env);
    int had_err = ip->had_error;
    if (had_err) fprintf(stderr, "\033[31mError: %s\033[0m\n", ip->errbuf);
    interp_free(ip);
    env_unref(env);
    node_free(ast);
    lexer_free(l);
    return had_err ? 1 : 0;
}

static void repl(void) {
    printf("\033[1;33m ██╗  ██╗██╗████████╗███████╗\n"
           " ██║ ██╔╝██║╚══██╔══╝██╔════╝\n"
           " █████╔╝ ██║   ██║   █████╗  \n"
           " ██╔═██╗ ██║   ██║   ██╔══╝  \n"
           " ██║  ██╗██║   ██║   ███████╗\n"
           " ╚═╝  ╚═╝╚═╝   ╚═╝   ╚══════╝\n\033[0m"
           "\033[90m  minimal language v1.2"
           "  |  .help  .env  .quit\033[0m\n\n");

    Env *env = env_new(NULL);
    setup_builtins(env);
    char line_buf[4096];
    char multi[65536]; multi[0] = '\0';
    int in_block = 0;

    while (1) {
        printf(in_block ? "\033[90m...  \033[0m" : "\033[32mkite>\033[0m ");
        fflush(stdout);
        if (!fgets(line_buf, (int)sizeof(line_buf), stdin)) break;
        int len = (int)strlen(line_buf);
        if (len > 0 && line_buf[len-1] == '\n') line_buf[len-1] = '\0';

        if (strcmp(line_buf,".quit")==0||strcmp(line_buf,".exit")==0) break;
        if (strcmp(line_buf,".help")==0) {
            printf("\033[36mKITE:\033[0m  set x=42  |  say(\"hi ${x}\")  |  def f(a): give a end\n"
                   "       when x>0: ... orwhen ...: ... else: ... end\n"
                   "       loop while cond: ... end  |  loop for v in list: ... end\n"
                   "       x -> x*x  (lambda)  |  break / next\n");
            continue;
        }
        if (strcmp(line_buf,".env")==0) {
            for (int i=0;i<ENV_HASH;i++) {
                EnvBucket *b=env->buckets[i];
                while(b){
                    if(b->val->type!=VT_BUILTIN){
                        printf("  %s = ",b->name);
                        val_print(b->val,stdout);
                        printf("\n");
                    }
                    b=b->next;
                }
            }
            continue;
        }

        if (strlen(multi)+strlen(line_buf)+2 < sizeof(multi)) {
            strcat(multi,line_buf); strcat(multi,"\n");
        }
        for (const char *p=line_buf; *p; p++) {
            if(strncmp(p,"def ",4)==0||strncmp(p,"when ",5)==0||strncmp(p,"loop ",5)==0) in_block++;
            if(strncmp(p,"end",3)==0&&(p[3]=='\0'||p[3]==' '||p[3]=='\n'||p[3]=='\r')) in_block--;
            if(in_block<0) in_block=0;
        }
        if (in_block==0 && strlen(multi)>0) {
            Lexer *l=lexer_new(multi); lexer_tokenize(l);
            Node *ast=parse(l);
            if (ast) {
                Interp *ip=interp_new();
                eval(ip,ast,env);
                if(ip->had_error) fprintf(stderr,"\033[31mError: %s\033[0m\n",ip->errbuf);
                interp_free(ip); node_free(ast);
            }
            lexer_free(l); multi[0]='\0';
        }
    }
    env_unref(env);
    printf("\033[90mbye!\033[0m\n");
}

int main(int argc, char *argv[]) {
    if (argc==1) { repl(); return 0; }
    int verbose=0; const char *filename=NULL;
    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"--tokens")==0)  verbose=1;
        else if(strcmp(argv[i],"--version")==0){ printf("kite 1.2.0\n"); return 0; }
        else if(strcmp(argv[i],"--help")==0){
            printf("Usage: kite [file.kite] [--tokens] [--version]\n"
                   "       kite   (REPL, reads stdin if no file)\n"); return 0;
        } else filename=argv[i];
    }
    char *src = filename ? read_file(filename) : read_stdin();
    if (!src) return 1;
    int ret = run_source(src, verbose);
    free(src); return ret;
}
