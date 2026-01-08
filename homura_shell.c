#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <ctype.h>

#define MAX_ARGS 128
#define MAX_JOBS 64

typedef struct {
    char **argv;
    int argc;
    char *infile;
    char *outfile;
    int append;
} Cmd;

typedef struct {
    Cmd *cmds;
    int n;
} Pipeline;

typedef struct {
    pid_t pid;
    char *cmdline;
} Job;

static Job jobs[MAX_JOBS];
static int job_count = 0;

static char **history = NULL;
static int hist_count = 0;
static char *hist_path = NULL;

static void sigint_handler(int sig) {
    (void)sig;
    write(STDOUT_FILENO, "\n", 1);
}

static void add_history(const char *line) {
    history = realloc(history, (hist_count + 1) * sizeof(char *));
    history[hist_count++] = strdup(line);
    if (hist_path) {
        FILE *f = fopen(hist_path, "a");
        if (f) { fprintf(f, "%s\n", line); fclose(f); }
    }
}

static void print_history(void) {
    for (int i = 0; i < hist_count; i++) {
        printf("%d: %s\n", i + 1, history[i]);
    }
}

static char *get_hist_path(void) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    size_t len = strlen(home) + strlen("/.homura_search_history") + 1;
    char *p = malloc(len);
    snprintf(p, len, "%s/.homura_search_history", home);
    return p;
}

static int setup_redir(Cmd *c, int rfd, int wfd) {
    if (c->infile) {
        int fd = open(c->infile, O_RDONLY);
        if (fd < 0) { perror("open infile"); return -1; }
        dup2(fd, STDIN_FILENO);
        close(fd);
    } else if (rfd != -1) {
        dup2(rfd, STDIN_FILENO);
    }
    if (c->outfile) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->outfile, flags, 0644);
        if (fd < 0) { perror("open outfile"); return -1; }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    } else if (wfd != -1) {
        dup2(wfd, STDOUT_FILENO);
    }
    return 0;
}

static int builtin(Cmd *c) {
    if (c->argc == 0) return 1;
    if (strcmp(c->argv[0], "exit") == 0) exit(0);
    if (strcmp(c->argv[0], "cd") == 0) {
        const char *dir = c->argv[1] ? c->argv[1] : getenv("HOME");
        if (chdir(dir) < 0) perror("cd");
        return 1;
    }
    if (strcmp(c->argv[0], "history") == 0) {
        print_history();
        return 1;
    }
    return 0;
}

static void exec_pipeline(Pipeline *p, int background, const char *cmdline) {
    int n = p->n;
    int pipes[2*(n-1)];
    for (int i = 0; i < n-1; i++) {
        if (pipe(&pipes[2*i]) < 0) perror("pipe");
    }
    pid_t pids[n];
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            int rfd = (i == 0) ? -1 : pipes[2*(i-1)];
            int wfd = (i == n-1) ? -1 : pipes[2*i+1];
            setup_redir(&p->cmds[i], rfd, wfd);
            for (int j = 0; j < 2*(n-1); j++) close(pipes[j]);
            execvp(p->cmds[i].argv[0], p->cmds[i].argv);
            perror("execvp");
            _exit(127);
        } else {
            pids[i] = pid;
        }
    }
    for (int j = 0; j < 2*(n-1); j++) close(pipes[j]);
    if (background) {
        jobs[job_count].pid = pids[n-1];
        jobs[job_count].cmdline = strdup(cmdline);
        job_count++;
    } else {
        for (int i = 0; i < n; i++) waitpid(pids[i], NULL, 0);
    }
}

static Pipeline parse_line(char *line) {
    Pipeline p = {0};
    p.cmds = calloc(1, sizeof(Cmd));
    p.n = 1;
    Cmd *c = &p.cmds[0];
    c->argv = calloc(MAX_ARGS, sizeof(char *));
    char *tok = strtok(line, " ");
    while (tok) {
        if (strcmp(tok, "|") == 0) {
            c->argv[c->argc] = NULL;
            p.cmds = realloc(p.cmds, (p.n+1)*sizeof(Cmd));
            c = &p.cmds[p.n++];
            c->argv = calloc(MAX_ARGS, sizeof(char *));
            c->argc = 0;
        } else if (strcmp(tok, "<") == 0) {
            tok = strtok(NULL, " ");
            c->infile = strdup(tok);
        } else if (strcmp(tok, ">") == 0) {
            tok = strtok(NULL, " ");
            c->outfile = strdup(tok);
            c->append = 0;
        } else if (strcmp(tok, ">>") == 0) {
            tok = strtok(NULL, " ");
            c->outfile = strdup(tok);
            c->append = 1;
        } else {
            c->argv[c->argc++] = strdup(tok);
        }
        tok = strtok(NULL, " ");
    }
    c->argv[c->argc] = NULL;
    return p;
}

int main(void) {
    signal(SIGINT, sigint_handler);
    hist_path = get_hist_path();
    char *line = NULL;
    size_t cap = 0;
    while (1) {
        printf("homura$ ");
        fflush(stdout);
        ssize_t n = getline(&line, &cap, stdin);
        if (n == -1) break;
        if (n && line[n-1] == '\n') line[n-1] = '\0';
        if (!*line) continue;
        add_history(line);
        int background = 0;
        if (line[strlen(line)-1] == '&') {
            background = 1;
            line[strlen(line)-1] = '\0';
        }
        Pipeline p = parse_line(line);
        if (!builtin(&p.cmds[0])) {
            exec_pipeline(&p, background, line);
        }
        for (int i = 0; i < p.n; i++) {
            for (int j = 0; j < p.cmds[i].argc; j++) free(p.cmds[i].argv[j]);
            free(p.cmds[i].argv);
            free(p.cmds[i].infile);
            free(p.cmds[i].outfile);
        }
        free(p.cmds);
    }
    free(line);
    return 0;
}
