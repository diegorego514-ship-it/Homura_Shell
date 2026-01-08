#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>

#define MAX_ARGS 256
#define MAX_JOBS 128

typedef struct {
    char **argv;
    int argc;
    char *infile;
    char *outfile;
    int append; // 1 for >>, 0 for >
} Cmd;

typedef struct {
    Cmd *v;
    int n;
    int cap;
} Pipeline;

typedef struct {
    char **toks;
    int n;
    int cap;
} Tokens;

typedef struct {
    pid_t pid;
    char *cmdline;
} Job;

typedef struct {
    Job jobs[MAX_JOBS];
    int count;
} JobList;

typedef struct {
    char **lines;
    size_t n;
    size_t cap;
    char *path;
} History;

// ---------- Globals (mirroring Python script) ----------
static History g_hist = {0};
static JobList g_jobs = {0};
static int g_last_status = 0;

// Aliases (Python ALIASES)
typedef struct { const char *key; const char *value; } Pair;
static const Pair ALIASES[] = {
    {"install", "fvp install"},
    {"remove",  "fvp remove"},
    {"update",  "fvp update"},
    {"upgrade", "fvp upgrade"},
    {"search",  "fvp search"},
    {"install", "fvp install"} // duplicate as in Python
};
static const int ALIASES_COUNT = sizeof(ALIASES)/sizeof(ALIASES[0]);

// SAVE_HISTORY_DATA (Python constant)
static const Pair SAVE_HISTORY_DATA[] = {
    {"folders", "directories"},
    {"networks", "ipv4"},
    {"browser-history", "user-history"},
    {"user-data", "data-packets"},
    {"user-browsing", "user-system-data"},
    {"user-system-info", "user-credentials"},
    {"user-system-passwords", "user-important-data"}
};
static const int SAVE_HISTORY_DATA_COUNT = sizeof(SAVE_HISTORY_DATA)/sizeof(SAVE_HISTORY_DATA[0]);

// ---------- Utilities ----------
static void sigint_handler(int signo) {
    (void)signo;
    write(STDOUT_FILENO, "\n", 1);
}

static void sigtstp_handler(int signo) {
    (void)signo;
    write(STDOUT_FILENO, "\n", 1);
}

static void free_cmd(Cmd *c) {
    if (!c) return;
    if (c->argv) {
        for (int i = 0; c->argv[i]; i++) free(c->argv[i]);
        free(c->argv);
    }
    free(c->infile);
    free(c->outfile);
    memset(c, 0, sizeof(*c));
}

static void free_pipeline(Pipeline *p) {
    if (!p || !p->v) return;
    for (int i = 0; i < p->n; i++) free_cmd(&p->v[i]);
    free(p->v);
    p->v = NULL; p->n = p->cap = 0;
}

static int pipeline_push(Pipeline *p, Cmd cmd) {
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 4;
        Cmd *nv = realloc(p->v, nc * sizeof(Cmd));
        if (!nv) return -1;
        p->v = nv; p->cap = nc;
    }
    p->v[p->n++] = cmd;
    return 0;
}

static void tokens_free(Tokens *t) {
    if (!t) return;
    for (int i = 0; i < t->n; i++) free(t->toks[i]);
    free(t->toks);
    t->toks = NULL; t->n = t->cap = 0;
}

static int push_tok(Tokens *t, const char *start, size_t len) {
    if (len == 0) return 0;
    char *s = malloc(len + 1);
    if (!s) return -1;
    memcpy(s, start, len);
    s[len] = '\0';
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 16;
        char **nv = realloc(t->toks, nc * sizeof(char *));
        if (!nv) { free(s); return -1; }
        t->toks = nv; t->cap = nc;
    }
    t->toks[t->n++] = s;
    return 0;
}

static int tokenize(const char *line, Tokens *out) {
    out->toks = NULL; out->n = out->cap = 0;
    const char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '|' || *p == '<' || *p == '>') {
            if (*p == '>' && *(p+1) == '>') { if (push_tok(out, ">>", 2) < 0) return -1; p += 2; continue; }
            if (push_tok(out, p, 1) < 0) return -1; p++; continue;
        }
        // word with quotes/escapes
        char *buf = NULL; size_t blen = 0, bcap = 0;
        #define BUF_PUSH(ch) do { \
            if (blen + 1 >= bcap) { size_t nc = bcap ? bcap * 2 : 64; char *nb = realloc(buf, nc); if (!nb) { free(buf); return -1; } buf = nb; bcap = nc; } \
            buf[blen++] = (ch); \
        } while(0)
        while (*p && !isspace((unsigned char)*p)) {
            if (*p == '|' || *p == '<' || *p == '>') break;
            if (*p == '\'') {
                p++;
                while (*p && *p != '\'') { BUF_PUSH(*p); p++; }
                if (*p == '\'') p++;
            } else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && *(p+1)) { p++; BUF_PUSH(*p); p++; }
                    else { BUF_PUSH(*p); p++; }
                }
                if (*p == '"') p++;
            } else if (*p == '\\' && *(p+1)) {
                p++; BUF_PUSH(*p); p++;
            } else {
                BUF_PUSH(*p); p++;
            }
        }
        BUF_PUSH('\0');
        int r = push_tok(out, buf, blen - 1);
        free(buf);
        if (r < 0) return -1;
        #undef BUF_PUSH
    }
    return 0;
}

static int build_pipeline(const Tokens *t, Pipeline *p) {
    p->v = NULL; p->n = p->cap = 0;
    Cmd cur = {0};
    cur.argv = calloc(MAX_ARGS, sizeof(char *));
    if (!cur.argv) return -1;
    cur.argc = 0;

    for (int i = 0; i < t->n; i++) {
        const char *tok = t->toks[i];
        if (strcmp(tok, "|") == 0) {
            if (cur.argc == 0) { fprintf(stderr, "syntax error: empty command before pipe\n"); free_cmd(&cur); return -1; }
            cur.argv[cur.argc] = NULL;
            if (pipeline_push(p, cur) < 0) { free_cmd(&cur); return -1; }
            cur.argv = calloc(MAX_ARGS, sizeof(char *));
            if (!cur.argv) return -1;
            cur.argc = 0; free(cur.infile); cur.infile = NULL; free(cur.outfile); cur.outfile = NULL; cur.append = 0;
        } else if (strcmp(tok, "<") == 0 || strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0) {
            if (i + 1 >= t->n) { fprintf(stderr, "syntax error: redirection without target\n"); free_cmd(&cur); return -1; }
            const char *fname = t->toks[++i];
            if (strcmp(tok, "<") == 0) { free(cur.infile); cur.infile = strdup(fname); }
            else { free(cur.outfile); cur.outfile = strdup(fname); cur.append = (strcmp(tok, ">>") == 0); }
        } else {
            if (cur.argc >= MAX_ARGS - 1) { fprintf(stderr, "too many arguments\n"); free_cmd(&cur); return -1; }
            cur.argv[cur.argc++] = strdup(tok);
        }
    }
    if (cur.argc > 0 || cur.infile || cur.outfile) {
        cur.argv[cur.argc] = NULL;
        if (pipeline_push(p, cur) < 0) { free_cmd(&cur); return -1; }
    } else {
        free_cmd(&cur);
    }
    if (p->n == 0) { fprintf(stderr, "empty pipeline\n"); return -1; }
    return 0;
}

static int setup_redirections(const Cmd *c, int read_fd, int write_fd) {
    if (c->infile) {
        int fd = open(c->infile, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "%s: %s\n", c->infile, strerror(errno)); return -1; }
        if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2"); close(fd); return -1; }
        close(fd);
    } else if (read_fd != -1) {
        if (dup2(read_fd, STDIN_FILENO) < 0) { perror("dup2"); return -1; }
    }
    if (c->outfile) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->outfile, flags, 0644);
        if (fd < 0) { fprintf(stderr, "%s: %s\n", c->outfile, strerror(errno)); return -1; }
        if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); close(fd); return -1; }
        close(fd);
    } else if (write_fd != -1) {
        if (dup2(write_fd, STDOUT_FILENO) < 0) { perror("dup2"); return -1; }
    }
    return 0;
}

static int command_exists(const char *cmd) {
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = strdup(path);
    if (!copy) return 0;
    char *save = NULL;
    char *dir = strtok_r(copy, ":", &save);
    while (dir) {
        size_t len = strlen(dir) + 1 + strlen(cmd) + 1;
        char *full = malloc(len);
        if (!full) { free(copy); return 0; }
        snprintf(full, len, "%s/%s", dir, cmd);
        if (access(full, X_OK) == 0) { free(full); free(copy); return 1; }
        free(full);
        dir = strtok_r(NULL, ":", &save);
    }
    free(copy);
    return 0;
}

static char *history_path(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    size_t len = strlen(home) + strlen("/.homura_search_history") + 1;
    char *p = malloc(len);
    if (!p) return NULL;
    snprintf(p, len, "%s/.homura_search_history", home);
    return p;
}

static void history_init(History *h) {
    h->lines = NULL; h->n = h->cap = 0; h->path = history_path();
    if (!h->path) return;
    FILE *f = fopen(h->path, "r");
    if (!f) return;
    char *line = NULL; size_t cap = 0; ssize_t n;
    while ((n = getline(&line, &cap, f)) != -1) {
        if (n && line[n-1] == '\n') line[n-1] = '\0';
        if (!*line) continue;
        if (h->n == h->cap) {
            size_t nc = h->cap ? h->cap * 2 : 64;
            char **nl = realloc(h->lines, nc * sizeof(char*));
            if (!nl) break;
            h->lines = nl; h->cap = nc;
        }
        h->lines[h->n++] = strdup(line);
    }
    free(line);
    fclose(f);
}

static void history_add(History *h, const char *line) {
    if (!line || !*line) return;
    if (h->n == h->cap) {
        size_t nc = h->cap ? h->cap * 2 : 64;
        char **nl = realloc(h->lines, nc * sizeof(char*));
        if (!nl) return;
        h->lines = nl; h->cap = nc;
    }
    h->lines[h->n++] = strdup(line);
    if (h->path) {
        FILE *f = fopen(h->path, "a");
        if (f) { fprintf(f, "%s\n", line); fclose(f); }
    }
}

static void history_print(const History *h) {
    for (size_t i = 0; i < h->n; i++) printf("%zu: %s\n", i + 1, h->lines[i]);
}

static void history_free(History *h) {
    if (!h) return;
    for (size_t i = 0; i < h->n; i++) free(h->lines[i]);
    free(h->lines);
    free(h->path);
    h->lines = NULL; h->path = NULL; h->n = h->cap = 0;
}

// ---------- Alias expansion ----------
static int expand_alias(Tokens *t) {
    if (t->n == 0) return 0;
    const char *first = t->toks[0];
    const char *value = NULL;
    for (int i = 0; i < ALIASES_COUNT; i++) {
        if (strcmp(ALIASES[i].key, first) == 0) { value = ALIASES[i].value; break; }
    }
    if (!value) return 0;
    // tokenize value and prepend
    Tokens tmp = {0};
    if (tokenize(value, &tmp) < 0) return -1;
    int newCount = tmp.n + (t->n - 1);
    char **newtoks = calloc(newCount, sizeof(char*));
    if (!newtoks) { tokens_free(&tmp); return -1; }
    int idx = 0;
    for (int i = 0; i < tmp.n; i++) newtoks[idx++] = strdup(tmp.toks[i]);
    for (int i = 1; i < t->n; i++) newtoks[idx++] = strdup(t->toks[i]);
    tokens_free(t);
    t->toks = newtoks; t->n = newCount; t->cap = newCount;
    tokens_free(&tmp);
    return 0;
}

// ---------- Builtins ----------
static int builtin(Pipeline *pipe) {
    if (!pipe || pipe->n < 1 || pipe->v[0].argc == 0) return 1; // treat empty as handled
    Cmd *c = &pipe->v[0];
    char **argv = c->argv;
    const char *cmd = argv[0];

    // builtins
    if (strcmp(cmd, "builtins") == 0) {
        printf("Added builtins in the new Shell Terminal Version Upgrade\n");
        return 1;
    }
    if (strcmp(cmd, "save-history-data") == 0) {
        printf("This is for saving history data\n");
        return 1;
    }
    if (strcmp(cmd, "path") == 0) {
        const char *dest = argv[1] ? argv[1] : getenv("HOME");
        if (!dest) dest = "/";
        if (chdir(dest) == -1) fprintf(stderr, "cd: %s: %s\n", dest, strerror(errno));
        return 1;
    }
    if (strcmp(cmd, "check") == 0 && argv[1] && strcmp(argv[1], "dir") == 0) {
        char buf[4096];
        if (getcwd(buf, sizeof(buf))) printf("%s\n", buf);
        else perror("getcwd");
        return 1;
    }
    if (strcmp(cmd, "clear") == 0 && argv[1] && strcmp(argv[1], "commands") == 0) {
        char buf[4096];
        if (getcwd(buf, sizeof(buf))) printf("%s\n", buf);
        else perror("getcwd");
        return 1;
    }
    if (strcmp(cmd, "history") == 0) {
        history_print(&g_hist);
        return 1;
    }
    if (strcmp(cmd, "save") == 0 && argv[1] && strcmp(argv[1], "history") == 0) {
        for (int i = 0; i < SAVE_HISTORY_DATA_COUNT; i++) {
            printf("%d: %s\n", i + 1, SAVE_HISTORY_DATA[i].key);
        }
        return 1;
    }
    if (strcmp(cmd, "jobs") == 0) {
        for (int i = 0; i < g_jobs.count; i++) {
            printf("%d: %d: %s\n", i, (int)g_jobs.jobs[i].pid, "cmd");
        }
        return 1;
    }
    if (strcmp(cmd, "fg") == 0) {
        if (!argv[1]) { fprintf(stderr, "fg: usage: fg INDEX\n"); return 1; }
        int idx = atoi(argv[1]);
        if (idx < 0 || idx >= g_jobs.count) { fprintf(stderr, "fg: no such job\n"); return 1; }
        Job j = g_jobs.jobs[idx];
        // remove job from list
        for (int k = idx; k < g_jobs.count - 1; k++) g_jobs.jobs[k] = g_jobs.jobs[k+1];
        g_jobs.count--;
        kill(j.pid, SIGCONT);
        int wst = 0;
        if (waitpid(j.pid, &wst, 0) < 0) perror("waitpid");
        else if (WIFEXITED(wst)) g_last_status = WEXITSTATUS(wst);
        free(j.cmdline);
        return 1;
    }
    if (strcmp(cmd, "transfer") == 0) {
        for (int i = 1; argv[i]; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) continue;
            *eq = '\0';
            const char *k = argv[i];
            const char *v = eq + 1;
            if (setenv(k, v, 1) < 0) perror("setenv");
        }
        return 1;
    }
    if (strcmp(cmd, "deselect") == 0) {
        for (int i = 1; argv[i]; i++) {
            unsetenv(argv[i]);
        }
        return 1;
    }
    // Package managers
    if (strcmp(cmd, "grp") == 0 || strcmp(cmd, "grp-get") == 0 || strcmp(cmd, "grp") == 0 ||
        strcmp(cmd, "fvp") == 0 || strcmp(cmd, "fvp-get") == 0) {
        // let execution path run them; if unsupported, they'll error naturally
        return 0; // not a builtin, execute normally
    }
    if (strcmp(cmd, "exit") == 0 && argv[1] && strcmp(argv[1], "shell") == 0) {
        // mimic Python's "exit shell"
        // write full history file (already appended; no readline here)
        exit(0);
    }

    return 0; // not handled here
}

// ---------- Execution ----------
static int exec_pipeline(Pipeline *p, int background, const char *cmdline_for_job) {
    int n = p->n;
    int pipes_count = (n > 1) ? (n - 1) * 2 : 0;
    int *pipes = NULL;
    if (pipes_count) {
        pipes = malloc(pipes_count * sizeof(int));
        if (!pipes) { perror("malloc"); return 1; }
        for (int i = 0; i < n - 1; i++) {
            if (pipe(&pipes[i * 2]) < 0) { perror("pipe"); free(pipes); return 1; }
        }
    }
    pid_t *pids = calloc(n, sizeof(pid_t));
    if (!pids) { perror("calloc"); free(pipes); return 1; }

    for (int i = 0; i < n; i++) {
        int read_fd = (i == 0) ? -1 : pipes[(i - 1) * 2];
        int write_fd = (i == n - 1) ? -1 : pipes[i * 2 + 1];

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); free(pipes); free(pids); return 1; }
        if (pid == 0) {
            // Child
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            // Close unused pipe ends
            if (pipes_count) {
                for (int k = 0; k < pipes_count; k++) {
                    int fd = pipes[k];
                    if (fd == read_fd || fd == write_fd) continue;
                    close(fd);
                }
            }
            if (setup_redirections(&p->v[i], read_fd, write_fd) < 0) _exit(1);
            if (read_fd != -1) close(read_fd);
            if (write_fd != -1) close(write_fd);

            execvp(p->v[i].argv[0], p->v[i].argv);
            fprintf(stderr, "homura: command not found: %s\n", p->v[i].argv[0]);
            _exit(127);
        } else {
            pids[i] = pid;
            if (read_fd != -1) close(read_fd);
            if (write_fd != -1) close(write_fd);
        }
    }

    int status = 0;
    if (background) {
        if (g_jobs.count < MAX_JOBS) {
            g_jobs.jobs[g_jobs.count].pid = pids[n - 1];
            g_jobs.jobs[g_jobs.count].cmdline = strdup(cmdline_for_job ? cmdline_for_job : "");
            g_jobs.count++;
        } else {
            fprintf(stderr, "jobs: job list full\n");
        }
    } else {
        for (int i = 0; i < n; i++) {
            int wst = 0;
            if (waitpid(pids[i], &wst, 0) < 0) perror("waitpid");
            if (WIFEXITED(wst)) status = WEXITSTATUS(wst);
            else if (WIFSIGNALED(wst)) status = 128 + WTERMSIG(wst);
        }
        g_last_status = status;
    }
    free(pids);
    free(pipes);
    return status;
}

// ---------- Main loop ----------
int main(void) {
    // signals
    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    struct sigaction sb = {0};
    sb.sa_handler = sigtstp_handler;
    sigemptyset(&sb.sa_mask);
    sb.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sb, NULL);

    history_init(&g_hist);

    char *line = NULL;
    size_t cap = 0;

    for (;;) {
        fputs("homura$ ", stdout);
        fflush(stdout);
        ssize_t n = getline(&line, &cap, stdin);
        if (n == -1) {
            if (feof(stdin)) { fputs("\n", stdout); break; }
            perror("getline");
            continue;
        }
        if (n && line[n-1] == '\n') line[n-1] = '\0';
        // skip empty
        const char *scan = line;
        while (*scan && isspace((unsigned char)*scan)) scan++;
        if (!*scan) continue;

        // history
        history_add(&g_hist, line);

        // tokenize
        Tokens toks = {0};
        if (tokenize(line, &toks) < 0) { fprintf(stderr, "parse error\n"); tokens_free(&toks); continue; }

        // background '&' at end
        int background = 0;
        if (toks.n > 0 && strcmp(toks.toks[toks.n - 1], "&") == 0) {
            background = 1;
            free(toks.toks[toks.n - 1]);
            toks.toks[--toks.n] = NULL;
        }

        // alias expansion
        if (expand_alias(&toks) < 0) { fprintf(stderr, "alias expansion failed\n"); tokens_free(&toks); continue; }

        // build pipeline
        // split on '|': we already keep '|' as tokens; build_pipeline will handle it with redir
        Pipeline pipe = {0};
        if (build_pipeline(&toks, &pipe) < 0) { tokens_free(&toks); free_pipeline(&pipe); continue; }
        tokens_free(&toks);

        // builtins run only if single command without pipes
        int handled = 0;
        if (pipe.n == 1) {
            handled = builtin(&pipe);
        }
        if (!handled) {
            exec_pipeline(&pipe, background, line);
        }

        free_pipeline(&pipe);
    }

    free(line);
    history_free(&g_hist);
    return 0;
}
