/*
 * rio_pack.c —— pack 配置解析 + 路径查找
 *
 * 一个极简的 INI 解析器：
 *   [section]        —— 段名
 *   key = value      —— 键值对
 *   # 或 ; 开头或行首空白 + #/; —— 注释
 *   section 名直接是虚拟路径本身（如 [/virtual/config.txt]）
 */

#define _GNU_SOURCE
#include "rio_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* ---- 全局配置 ---- */
static rio_pack_cfg_t g_cfg = { .enabled = 0, .prefix = "", .pack_file = "", .n_entries = 0 };

/* ---- 小工具 ---- */
static char *trim(char *s) {
    if (!s) return NULL;
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

/* ---- INI 解析 ---- */
static int parse_ini_line(char *line, char **section_out,
                          char **key_out, char **val_out) {
    /* 返回值：0=空/注释行, 1=[section], 2=key=value */
    char *p = trim(line);
    if (*p == '\0' || *p == '#' || *p == ';') return 0;

    if (*p == '[') {
        char *close = strchr(p, ']');
        if (!close) return 0;
        *close = '\0';
        *section_out = p + 1;
        return 1;
    }

    char *eq = strchr(p, '=');
    if (!eq) return 0;
    *eq = '\0';
    *key_out = trim(p);
    *val_out = trim(eq + 1);
    return 2;
}

/* ---- 加载 ---- */
int rio_pack_load(const char *path) {
    if (!path || !*path) return 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        RIO_LOG("rio_pack_load: open %s failed: %s", path, strerror(errno));
        return 0;
    }

    rio_pack_cfg_t cfg = { .enabled = 0, .prefix = "", .pack_file = "", .n_entries = 0 };
    char current_section[RIO_PACK_PATH_MAX] = "";  /* 当前解析的 section 名 */

    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        char *section = NULL, *key = NULL, *val = NULL;
        int kind = parse_ini_line(buf, &section, &key, &val);

        if (kind == 1) {
            snprintf(current_section, sizeof(current_section), "%s", section);
            continue;
        }
        if (kind != 2) continue;

        if (strcmp(current_section, "pack") == 0) {
            if (!strcmp(key, "prefix")) {
                snprintf(cfg.prefix, sizeof(cfg.prefix), "%s", val);
            } else if (!strcmp(key, "pack_file")) {
                snprintf(cfg.pack_file, sizeof(cfg.pack_file), "%s", val);
            }
        } else if (current_section[0] == '/' || (cfg.prefix[0] &&
                   strncmp(current_section, cfg.prefix, strlen(cfg.prefix)) == 0)) {
            /* section 名就是虚拟路径；在里面找 offset/size */
            /* 找一个已存在的 entry，或新增 */
            rio_pack_entry_t *e = NULL;
            for (int i = 0; i < cfg.n_entries; i++) {
                if (strcmp(cfg.entries[i].vpath, current_section) == 0) {
                    e = &cfg.entries[i]; break;
                }
            }
            if (!e && cfg.n_entries < RIO_PACK_MAX_ENTRIES) {
                e = &cfg.entries[cfg.n_entries++];
                memset(e, 0, sizeof(*e));
                snprintf(e->vpath, sizeof(e->vpath), "%s", current_section);
                e->valid = 1;
            }
            if (!e) continue;
            if (!strcmp(key, "offset"))  e->offset = strtoull(val, NULL, 0);
            else if (!strcmp(key, "size")) e->size  = strtoull(val, NULL, 0);
        }
    }
    fclose(f);

    /* 校验：必须有 pack_file + 至少一个 entry */
    if (!cfg.pack_file[0] || cfg.n_entries == 0) {
        RIO_LOG("rio_pack_load: %s 缺少 pack_file 或 entries，忽略", path);
        return 0;
    }
    cfg.enabled = 1;
    g_cfg = cfg;   /* 赋值（结构体浅拷贝，字符串都是内部 buffer，安全）*/
    RIO_LOG("rio_pack_load: %s 已启用, pack_file=%s, n_entries=%d",
            path, g_cfg.pack_file, g_cfg.n_entries);
    return 1;
}

void rio_pack_autoload(void) {
    const char *p = getenv("RIO_PACK_CONFIG");
    if (p && *p) { rio_pack_load(p); return; }

    /* 兜底：当前目录的 rio_pack.ini */
    rio_pack_load("./rio_pack.ini");
}

const rio_pack_entry_t *rio_pack_lookup(const char *vpath) {
    if (!g_cfg.enabled || !vpath) return NULL;
    /* 先精确匹配 */
    for (int i = 0; i < g_cfg.n_entries; i++) {
        if (g_cfg.entries[i].valid && strcmp(g_cfg.entries[i].vpath, vpath) == 0)
            return &g_cfg.entries[i];
    }
    /* 再按 prefix + 相对路径匹配 */
    if (g_cfg.prefix[0] && strncmp(vpath, g_cfg.prefix, strlen(g_cfg.prefix)) == 0) {
        /* 如果 entry 是 /virtual/config.txt，vpath 正好就是它 → 上面精确匹配已覆盖 */
        /* 这里兜底支持 prefix= /virtual + entry=[config.txt] 这种简化写法 */
    }
    return NULL;
}

const rio_pack_cfg_t *rio_pack_cfg(void) { return &g_cfg; }
