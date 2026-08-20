/* ============================================================================
 * NexxoN OS - /etc/hosts resolver (NXFS-backed, no per-app hardcoding)
 * ============================================================================ */
#include "net.h"
#include "nxfs.h"
#include "string.h"
#include "string.h"
#include "debug.h"

#define HOSTS_MAX   64
#define HOSTS_LINE    256
#define HOSTS_FILE  "/etc/hosts"

typedef struct {
    uint32_t ip;
    char     name[64];
} hosts_entry_t;

static hosts_entry_t g_hosts[HOSTS_MAX];
static uint32_t      g_hosts_n;
static bool          g_hosts_loaded;

static void hosts_reset(void) {
    g_hosts_n = 0;
    memset(g_hosts, 0, sizeof(g_hosts));
}

static bool hosts_skip_line(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return !*p || *p == '#';
}

static int hosts_push(uint32_t ip, const char *name) {
    if (!name || !name[0] || g_hosts_n >= HOSTS_MAX)
        return -1;
    for (uint32_t i = 0; i < g_hosts_n; i++) {
        if (strcmp(g_hosts[i].name, name) == 0) {
            g_hosts[i].ip = ip;
            return 0;
        }
    }
    g_hosts[g_hosts_n].ip = ip;
    ksnprintf(g_hosts[g_hosts_n].name, sizeof(g_hosts[g_hosts_n].name),
              "%s", name);
    g_hosts_n++;
    return 0;
}

static void hosts_parse_line(char *line) {
    if (hosts_skip_line(line))
        return;
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    char *ip_tok = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '#') p++;
    if (*p) { *p = 0; p++; }
    uint32_t ip = net_ip_aton(ip_tok);
    if (!ip)
        return;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#')
            break;
        char *name = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '#') p++;
        if (*p) *p++ = 0;
        hosts_push(ip, name);
    }
}

void net_hosts_reload(void) {
    hosts_reset();
    uint32_t ino = 0;
    if (nxfs_resolve_path(HOSTS_FILE, &ino) != NXFS_OK) {
        g_hosts_loaded = true;
        return;
    }
    static char buf[4096];
    uint32_t got = 0;
    if (nxfs_read_file(ino, buf, sizeof(buf) - 1, &got) != NXFS_OK || !got) {
        g_hosts_loaded = true;
        return;
    }
    buf[got] = 0;
    char line[HOSTS_LINE];
    uint32_t li = 0;
    for (uint32_t i = 0; i <= got; i++) {
        char c = (i < got) ? buf[i] : '\n';
        if (c == '\n' || c == '\r') {
            line[li] = 0;
            hosts_parse_line(line);
            li = 0;
        } else if (li + 1 < sizeof(line)) {
            line[li++] = c;
        }
    }
    g_hosts_loaded = true;
    debug_printf("[net/hosts] loaded %u entries from %s\n",
                 g_hosts_n, HOSTS_FILE);
}

int net_hosts_lookup(const char *host, uint32_t *out_ip) {
    if (!host || !out_ip)
        return -1;
    if (!g_hosts_loaded)
        net_hosts_reload();
    for (uint32_t i = 0; i < g_hosts_n; i++) {
        if (strcmp(g_hosts[i].name, host) == 0) {
            *out_ip = g_hosts[i].ip;
            return 0;
        }
    }
    return -1;
}
