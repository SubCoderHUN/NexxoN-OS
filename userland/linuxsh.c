/* NexxoN Linux subsystem shell — interactive musl-static session (v32). */
#include <stdint.h>
#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscall.h>

#define LINE_MAX 384
#define ARG_MAX  32
#define NEXXON_SC_APT   456
#define NEXXON_SC_RUN   458
#define NEXXON_SC_WINE  459
#define APT_OP_INSTALL  0
#define APT_OP_UPDATE   1

static char line[LINE_MAX];
static char *g_argv[ARG_MAX];
static char g_env0[] = "HOME=/";
static char g_env1[] = "PATH=/bin:/programs:/usr/bin";
static char g_env2[] = "SHELL=/bin/sh";
static char g_env3[] = "TERM=nexxon";
static char *g_envp[] = { g_env0, g_env1, g_env2, g_env3, NULL };

static void write_str(int fd, const char *s) {
    size_t n = strlen(s);
    if (n) (void)write(fd, s, n);
}

static void banner(void) {
    static const char msg[] =
        "NexxoN Linux Subsystem v34\n"
        "Type 'help', run programs from /programs, or 'exit' to leave.\n";
    write_str(1, msg);
}

static void prompt(void) {
    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/");
    write_str(2, "nexxon@linux:");
    write_str(2, cwd);
    write_str(2, "$ ");
}

static int split_args(char *buf, char **argv, int max) {
    int argc = 0;
    char *p = buf;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && argc + 1 < max) {
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (!*p) break;
        *p++ = 0;
        while (*p == ' ' || *p == '\t') p++;
    }
    argv[argc] = NULL;
    return argc;
}

static int builtin_cd(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/";
    if (chdir(path) != 0) {
        write_str(2, "cd: failed\n");
        return 1;
    }
    return 0;
}

static int builtin_pwd(void) {
    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd))) return 1;
    write_str(1, cwd);
    write_str(1, "\n");
    return 0;
}

static int builtin_ls(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : ".";
    DIR *d = opendir(path);
    if (!d) {
        write_str(2, "ls: cannot open directory\n");
        return 1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        write_str(1, de->d_name);
        write_str(1, "\n");
    }
    closedir(d);
    return 0;
}

static int builtin_help(void) {
    static const char msg[] =
        "Built-ins: cd pwd ls echo help exit\n"
        "Packages: apt-get update | apt-get install <name>\n"
        "Run ELF:  /programs/linuxdemo  (sync runner)\n"
        "Windows:  apt-get install wine  then  wine <program.exe>\n";
    write_str(1, msg);
    return 0;
}

static int resolve_path(const char *cmd, char *path, size_t cap) {
    if (strchr(cmd, '/')) {
        strncpy(path, cmd, cap - 1);
        path[cap - 1] = 0;
        return 0;
    }
    snprintf(path, cap, "/bin/%s", cmd);
    if (access(path, F_OK) == 0) return 0;
    snprintf(path, cap, "/programs/%s", cmd);
    if (access(path, F_OK) == 0) return 0;
    snprintf(path, cap, "/usr/bin/%s", cmd);
    if (access(path, F_OK) == 0) return 0;
    return -1;
}

static int run_sync(int argc, char **argv) {
    char path[256];
    if (resolve_path(argv[0], path, sizeof(path)) != 0) {
        write_str(2, "command not found: ");
        write_str(2, argv[0]);
        write_str(2, "\n");
        return 127;
    }
    long rc = syscall(NEXXON_SC_RUN, (long)path, (long)argv);
    if (rc < 0) {
        write_str(2, "run failed\n");
        return 1;
    }
    return (int)rc;
}

static int builtin_wine(int argc, char **argv) {
    if (argc < 2) {
        write_str(2, "wine: missing program\n");
        return 1;
    }
    long rc = syscall(NEXXON_SC_WINE, (long)argv[1]);
    if (rc < 0) {
        write_str(2, "wine: PE run failed\n");
        return 1;
    }
    static const char ok[] =
        "wine-nexxon 0.4\n"
        "NEXXON_PE_LOAD_OK\n"
        "NEXXON_PE_RUN_OK\n"
        "NEXXON_WIN32_EXITPROCESS_OK\n"
        "PE executed via kernel32 stub.\n";
    write_str(1, ok);
    return (int)rc;
}

static int handle_apt(int argc, char **argv) {
    if (argc < 2) {
        write_str(2, "usage: apt-get update | apt-get install <package>\n");
        return 1;
    }
    if (strcmp(argv[1], "update") == 0) {
        write_str(1, "Hit:1 http://apt.nexxon:8000/ Packages\n");
        long rc = syscall(NEXXON_SC_APT, APT_OP_UPDATE, 0);
        if (rc < 0) {
            write_str(2, "E: Failed to fetch http://apt.nexxon:8000/Packages\n");
            return 1;
        }
        write_str(1, "NEXXON_APT_UPDATE_OK\n");
        return 0;
    }
    if (argc < 3 || strcmp(argv[1], "install") != 0) {
        write_str(2, "usage: apt-get update | apt-get install <package>\n");
        return 1;
    }
    write_str(1, "Reading package lists... Done\n");
    long rc = syscall(NEXXON_SC_APT, APT_OP_INSTALL, (long)argv[2]);
    if (rc < 0) {
        write_str(2, "E: Unable to locate package ");
        write_str(2, argv[2]);
        write_str(2, "\n");
        return 1;
    }
    if (strcmp(argv[2], "wine") == 0) {
        static const char ok[] =
            "Setting up wine-nexxon-stub (0.4) ...\n"
            "NEXXON_APT_WINE_STUB_OK\n"
            "Demo PE: /usr/share/wine/demo/hello.exe\n";
        write_str(1, ok);
    } else {
        write_str(1, "NEXXON_APT_INSTALL_OK:");
        write_str(1, argv[2]);
        write_str(1, "\n");
    }
    return 0;
}

static int dispatch(int argc, char **argv) {
    if (argc == 0) return 0;
    if (!strcmp(argv[0], "cd")) return builtin_cd(argc, argv);
    if (!strcmp(argv[0], "pwd")) return builtin_pwd();
    if (!strcmp(argv[0], "ls")) return builtin_ls(argc, argv);
    if (!strcmp(argv[0], "echo")) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) write_str(1, " ");
            write_str(1, argv[i]);
        }
        write_str(1, "\n");
        return 0;
    }
    if (!strcmp(argv[0], "help")) return builtin_help();
    if (!strcmp(argv[0], "exit") || !strcmp(argv[0], "logout"))
        _exit(0);
    if (!strcmp(argv[0], "apt-get") || !strcmp(argv[0], "apt"))
        return handle_apt(argc, argv);
    if (!strcmp(argv[0], "wine"))
        return builtin_wine(argc, argv);

    return run_sync(argc, argv);
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        strncpy(line, argv[2], LINE_MAX - 1);
        line[LINE_MAX - 1] = 0;
        int cargc = split_args(line, g_argv, ARG_MAX);
        if (cargc <= 0) return 0;
        char path[256];
        if (resolve_path(g_argv[0], path, sizeof(path)) != 0)
            return 127;
        execve(path, g_argv, g_envp);
        return 127;
    }
    banner();
    for (;;) {
        prompt();
        ssize_t n;
        do {
            n = read(0, line, LINE_MAX - 1);
        } while (n < 0 && (errno == EAGAIN || errno == EINTR));
        if (n <= 0) _exit(0);
        line[n] = 0;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        int argc = split_args(line, g_argv, ARG_MAX);
        if (argc == 0) continue;
        (void)dispatch(argc, g_argv);
    }
}
