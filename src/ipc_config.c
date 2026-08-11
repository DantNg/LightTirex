#include "ipc_config.h"
#include "ipc_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG_LOCK_MS 2000u
#define CFG_MAGIC   "#ltx-cfg 1"

typedef struct {
    char key[IPC_CFG_KEY_LEN];
    ipc_cfg_type_t type;
    int32_t i;
    char s[IPC_CFG_STR_LEN];
    bool in_use;
} entry_t;

static entry_t     s_entries[IPC_CFG_MAX_ENTRIES];
static ipc_cfg_cfg_t s_cfg;
static ipc_mutex_t s_lock;
static bool        s_dirty;
static ipc_timer_id_t s_save_timer = IPC_TIMER_NONE;
static ipc_handler_t  s_writer_h;
static bool           s_writer_ready;

#define CFG_MSG_SAVE 0xC0F91u

/* ------------------------------------------------------------------ */
/* Backend                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    ipc_cfg_storage_t base;
    const char *path;
    char tmp_path[128];
} file_storage_t;

static int file_load(ipc_cfg_storage_t *self, char *buf, uint32_t cap)
{
    file_storage_t *fs = (file_storage_t *)self->impl;
    FILE *f = fopen(fs->path, "rb");
    if (!f) return 0;   /* chua co file = chua luu lan nao, khong phai loi */
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

static int file_save(ipc_cfg_storage_t *self, const char *buf, uint32_t len)
{
    file_storage_t *fs = (file_storage_t *)self->impl;

    /* Ghi ban tam truoc. Mat dien giua chung thi file that van nguyen. */
    FILE *f = fopen(fs->tmp_path, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, len, f);
    if (fflush(f) != 0) { fclose(f); return -1; }
    if (fclose(f) != 0) return -1;
    if (w != len) { remove(fs->tmp_path); return -1; }

    /* rename() tren Windows that bai neu dich da ton tai -> xoa truoc.
     * Do la khe cua so duy nhat khong nguyen to; tren POSIX rename de len
     * la nguyen to nen ta thu truoc, chi xoa khi that bai. */
    if (rename(fs->tmp_path, fs->path) != 0) {
        remove(fs->path);
        if (rename(fs->tmp_path, fs->path) != 0) { remove(fs->tmp_path); return -1; }
    }
    return 0;
}

ipc_cfg_storage_t *ipc_cfg_storage_file(const char *path)
{
    static file_storage_t fs;
    fs.path = path;
    snprintf(fs.tmp_path, sizeof(fs.tmp_path), "%s.tmp", path);
    fs.base.name = "file";
    fs.base.load = file_load;
    fs.base.save = file_save;
    fs.base.impl = &fs;
    return &fs.base;
}

static int ram_load(ipc_cfg_storage_t *self, char *buf, uint32_t cap)
{
    ipc_cfg_ram_storage_t *rs = (ipc_cfg_ram_storage_t *)self->impl;
    rs->load_count++;
    uint32_t n = rs->len < cap - 1 ? rs->len : cap - 1;
    memcpy(buf, rs->buf, n);
    buf[n] = '\0';
    return (int)n;
}

static int ram_save(ipc_cfg_storage_t *self, const char *buf, uint32_t len)
{
    ipc_cfg_ram_storage_t *rs = (ipc_cfg_ram_storage_t *)self->impl;
    if (rs->fail_next_save) { rs->fail_next_save = false; return -1; }
    if (len >= sizeof(rs->buf)) return -1;
    memcpy(rs->buf, buf, len);
    rs->len = len;
    rs->buf[len] = '\0';
    rs->save_count++;
    return 0;
}

void ipc_cfg_storage_ram_init(ipc_cfg_ram_storage_t *rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->base.name = "ram";
    rs->base.load = ram_load;
    rs->base.save = ram_save;
    rs->base.impl = rs;
}

/* ------------------------------------------------------------------ */
/* Bang key/value                                                      */
/* ------------------------------------------------------------------ */

static entry_t *find(const char *key)
{
    if (!key) return NULL;
    for (int i = 0; i < IPC_CFG_MAX_ENTRIES; ++i) {
        if (s_entries[i].in_use &&
            strncmp(s_entries[i].key, key, IPC_CFG_KEY_LEN) == 0)
            return &s_entries[i];
    }
    return NULL;
}

static entry_t *find_or_add(const char *key, ipc_cfg_type_t type)
{
    entry_t *e = find(key);
    if (e) return e;
    for (int i = 0; i < IPC_CFG_MAX_ENTRIES; ++i) {
        if (!s_entries[i].in_use) {
            e = &s_entries[i];
            memset(e, 0, sizeof(*e));
            snprintf(e->key, sizeof(e->key), "%s", key);
            e->type = type;
            e->in_use = true;
            return e;
        }
    }
    return NULL;
}

void ipc_cfg_reset_defaults(void)
{
    if (s_lock && !ipc_mutex_lock(s_lock, CFG_LOCK_MS)) return;
    memset(s_entries, 0, sizeof(s_entries));
    for (uint32_t i = 0; i < s_cfg.schema_count; ++i) {
        const ipc_cfg_schema_t *sc = &s_cfg.schema[i];
        entry_t *e = find_or_add(sc->key, sc->type);
        if (!e) break;
        e->type = sc->type;
        e->i = sc->def_int;
        if (sc->type == IPC_CFG_STR)
            snprintf(e->s, sizeof(e->s), "%s", sc->def_str ? sc->def_str : "");
    }
    s_dirty = false;
    if (s_lock) ipc_mutex_unlock(s_lock);
}

/* ------------------------------------------------------------------ */
/* Tuan tu hoa                                                         */
/* ------------------------------------------------------------------ */

/* FNV-1a 32-bit: du de phat hien file cut/hong, re, khong can bang tra. */
static uint32_t checksum(const char *p, uint32_t len)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; ++i) {
        h ^= (uint8_t)p[i];
        h *= 16777619u;
    }
    return h;
}

/* Tra ve do dai, hoac so am neu bo dem khong du. */
static int serialize(char *buf, uint32_t cap)
{
    /* Than truoc, header sau (vi header chua checksum cua than).
     * static chu khong phai bien stack: 2KB tren stack task MCU la qua nhieu.
     * An toan vi moi duong vao deu di qua s_lock. */
    static char body[IPC_CFG_BUF_SIZE];
    uint32_t n = 0, count = 0;

    for (int i = 0; i < IPC_CFG_MAX_ENTRIES; ++i) {
        entry_t *e = &s_entries[i];
        if (!e->in_use) continue;
        int w;
        if (e->type == IPC_CFG_STR)
            w = snprintf(body + n, sizeof(body) - n, "%s\ts\t%s\n", e->key, e->s);
        else
            w = snprintf(body + n, sizeof(body) - n, "%s\t%c\t%ld\n", e->key,
                         e->type == IPC_CFG_BOOL ? 'b' : 'i', (long)e->i);
        if (w < 0 || (uint32_t)w >= sizeof(body) - n) return IPC_CFG_ERR_NO_SPACE;
        n += (uint32_t)w;
        count++;
    }

    int hw = snprintf(buf, cap, "%s %08x %u\n", CFG_MAGIC,
                      (unsigned)checksum(body, n), (unsigned)count);
    if (hw < 0 || (uint32_t)hw >= cap) return IPC_CFG_ERR_NO_SPACE;
    if ((uint32_t)hw + n >= cap) return IPC_CFG_ERR_NO_SPACE;
    memcpy(buf + hw, body, n);
    buf[hw + n] = '\0';
    return hw + (int)n;
}

static ipc_cfg_err_t deserialize(char *buf, uint32_t len)
{
    if (len == 0) return IPC_CFG_OK;   /* chua co du lieu: giu mac dinh */

    char *nl = memchr(buf, '\n', len);
    if (!nl) return IPC_CFG_ERR_CORRUPT;
    *nl = '\0';

    unsigned sum = 0, count = 0;
    if (strncmp(buf, CFG_MAGIC, strlen(CFG_MAGIC)) != 0)
        return IPC_CFG_ERR_CORRUPT;   /* khong phai file cua ta, hoac sai phien ban */
    if (sscanf(buf + strlen(CFG_MAGIC), " %8x %u", &sum, &count) != 2)
        return IPC_CFG_ERR_CORRUPT;

    char *body = nl + 1;
    uint32_t body_len = len - (uint32_t)(body - buf);
    if (checksum(body, body_len) != (uint32_t)sum)
        return IPC_CFG_ERR_CORRUPT;   /* hong -> nguoi goi giu nguyen mac dinh */

    uint32_t applied = 0;
    char *line = body;
    while (line && *line) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        if (*line) {
            char *t1 = strchr(line, '\t');
            char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            if (t1 && t2) {
                *t1 = '\0'; *t2 = '\0';
                const char *key = line;
                char type = *(t1 + 1);
                const char *val = t2 + 1;

                /* Chi nhan khoa da khai bao trong schema: file cu hoac file
                 * bi sua tay khong the bom khoa la vao he thong. */
                entry_t *e = find(key);
                if (e) {
                    if (type == 's' && e->type == IPC_CFG_STR) {
                        snprintf(e->s, sizeof(e->s), "%s", val);
                        applied++;
                    } else if ((type == 'i' && e->type == IPC_CFG_INT) ||
                               (type == 'b' && e->type == IPC_CFG_BOOL)) {
                        e->i = (int32_t)strtol(val, NULL, 10);
                        applied++;
                    }
                }
            }
        }
        line = end ? end + 1 : NULL;
    }
    (void)applied;
    return IPC_CFG_OK;
}

/* ------------------------------------------------------------------ */
/* Doc/ghi storage                                                     */
/* ------------------------------------------------------------------ */

ipc_cfg_err_t ipc_cfg_load(void)
{
    if (!s_cfg.storage || !s_cfg.storage->load) return IPC_CFG_ERR_ARG;
    if (s_lock && !ipc_mutex_lock(s_lock, CFG_LOCK_MS)) return IPC_CFG_ERR_IO;

    static char buf[IPC_CFG_BUF_SIZE];
    int n = s_cfg.storage->load(s_cfg.storage, buf, sizeof(buf));
    ipc_cfg_err_t rc;
    if (n < 0) rc = IPC_CFG_ERR_IO;
    else       rc = deserialize(buf, (uint32_t)n);

    if (rc == IPC_CFG_OK) s_dirty = false;
    if (s_lock) ipc_mutex_unlock(s_lock);
    return rc;
}

ipc_cfg_err_t ipc_cfg_save(void)
{
    if (!s_cfg.storage || !s_cfg.storage->save) return IPC_CFG_ERR_ARG;
    if (s_lock && !ipc_mutex_lock(s_lock, CFG_LOCK_MS)) return IPC_CFG_ERR_IO;

    static char buf[IPC_CFG_BUF_SIZE];
    int n = serialize(buf, sizeof(buf));
    ipc_cfg_err_t rc;
    if (n < 0) {
        rc = (ipc_cfg_err_t)n;
    } else if (s_cfg.storage->save(s_cfg.storage, buf, (uint32_t)n) != 0) {
        rc = IPC_CFG_ERR_IO;   /* giu nguyen co dirty de con thu lai */
    } else {
        s_dirty = false;
        rc = IPC_CFG_OK;
    }
    if (s_lock) ipc_mutex_unlock(s_lock);
    return rc;
}

bool ipc_cfg_is_dirty(void) { return s_dirty; }

/* Ghi tren looper duoc chi dinh, khong lam nghen task timer. */
static bool writer_cb(ipc_handler_t *h, ipc_message_t *m, void *user)
{
    (void)h; (void)user;
    if (m->what == CFG_MSG_SAVE) ipc_cfg_save();
    return true;
}

static void save_on_timer(void *arg)
{
    (void)arg;
    ipc_cfg_save();
}

void ipc_cfg_save_deferred(void)
{
    if (s_cfg.autosave_delay_ms == 0) { ipc_cfg_save(); return; }

    /* Debounce: moi lan set lai day han ghi ra xa. Mot tran 100 lan set lien
     * tiep chi ton DUNG MOT lan ghi flash. */
    if (s_save_timer != IPC_TIMER_NONE && ipc_timer_restart(s_save_timer, 0))
        return;

    if (s_writer_ready) {
        s_save_timer = ipc_timer_send_delayed(&s_writer_h, CFG_MSG_SAVE, 0,
                                              s_cfg.autosave_delay_ms);
    } else {
        s_save_timer = ipc_timer_call_after(save_on_timer, NULL,
                                            s_cfg.autosave_delay_ms);
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

void ipc_cfg_cfg_default(ipc_cfg_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->autosave_delay_ms = 500;
}

ipc_cfg_err_t ipc_cfg_init(const ipc_cfg_cfg_t *cfg)
{
    if (!cfg || !cfg->storage) return IPC_CFG_ERR_ARG;
    if (!s_lock) s_lock = ipc_mutex_create();
    s_cfg = *cfg;
    s_save_timer = IPC_TIMER_NONE;
    s_writer_ready = false;

    if (s_cfg.writer_looper) {
        ipc_handler_init(&s_writer_h, s_cfg.writer_looper, writer_cb, NULL, "cfg");
        s_writer_ready = true;
    }

    ipc_cfg_reset_defaults();
    return ipc_cfg_load();
}

int32_t ipc_cfg_get_int(const char *key, int32_t fallback)
{
    if (s_lock && !ipc_mutex_lock(s_lock, CFG_LOCK_MS)) return fallback;
    entry_t *e = find(key);
    int32_t v = (e && e->type == IPC_CFG_INT) ? e->i : fallback;
    if (s_lock) ipc_mutex_unlock(s_lock);
    return v;
}

bool ipc_cfg_get_bool(const char *key, bool fallback)
{
    if (s_lock && !ipc_mutex_lock(s_lock, CFG_LOCK_MS)) return fallback;
    entry_t *e = find(key);
    bool v = (e && e->type == IPC_CFG_BOOL) ? (e->i != 0) : fallback;
    if (s_lock) ipc_mutex_unlock(s_lock);
    return v;
}

int ipc_cfg_get_str(const char *key, char *out, uint32_t cap, const char *fallback)
{
    if (!out || cap == 0) return IPC_CFG_ERR_ARG;
    if (s_lock && !ipc_mutex_lock(s_lock, CFG_LOCK_MS)) return IPC_CFG_ERR_IO;
    entry_t *e = find(key);
    const char *src = (e && e->type == IPC_CFG_STR) ? e->s : (fallback ? fallback : "");
    int n = snprintf(out, cap, "%s", src);
    if (s_lock) ipc_mutex_unlock(s_lock);
    return n < 0 ? IPC_CFG_ERR_ARG : n;
}

static ipc_cfg_err_t set_common(const char *key, ipc_cfg_type_t type,
                                int32_t iv, const char *sv)
{
    if (!key) return IPC_CFG_ERR_ARG;
    if (s_lock && !ipc_mutex_lock(s_lock, CFG_LOCK_MS)) return IPC_CFG_ERR_IO;

    entry_t *e = find_or_add(key, type);
    if (!e) { if (s_lock) ipc_mutex_unlock(s_lock); return IPC_CFG_ERR_FULL; }
    if (e->type != type) { if (s_lock) ipc_mutex_unlock(s_lock); return IPC_CFG_ERR_TYPE; }

    bool changed;
    if (type == IPC_CFG_STR) {
        changed = strncmp(e->s, sv ? sv : "", IPC_CFG_STR_LEN) != 0;
        if (changed) snprintf(e->s, sizeof(e->s), "%s", sv ? sv : "");
    } else {
        changed = (e->i != iv);
        if (changed) e->i = iv;
    }
    if (changed) s_dirty = true;
    if (s_lock) ipc_mutex_unlock(s_lock);

    /* Ghi va bao thay doi lam NGOAI vung khoa: callback nguoi dung co the
     * goi nguoc lai ipc_cfg_get_* ma khong bi tu khoa chan. */
    if (changed) {
        if (s_cfg.on_change) s_cfg.on_change(key, s_cfg.user);
        ipc_cfg_save_deferred();
    }
    return IPC_CFG_OK;
}

ipc_cfg_err_t ipc_cfg_set_int(const char *key, int32_t v)
{ return set_common(key, IPC_CFG_INT, v, NULL); }

ipc_cfg_err_t ipc_cfg_set_bool(const char *key, bool v)
{ return set_common(key, IPC_CFG_BOOL, v ? 1 : 0, NULL); }

ipc_cfg_err_t ipc_cfg_set_str(const char *key, const char *v)
{ return set_common(key, IPC_CFG_STR, 0, v); }

bool ipc_cfg_has(const char *key) { return find(key) != NULL; }

uint32_t ipc_cfg_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < IPC_CFG_MAX_ENTRIES; ++i) if (s_entries[i].in_use) n++;
    return n;
}

void ipc_cfg_dump(void (*print)(const char *line))
{
    if (!print) return;
    char line[128];
    for (int i = 0; i < IPC_CFG_MAX_ENTRIES; ++i) {
        entry_t *e = &s_entries[i];
        if (!e->in_use) continue;
        if (e->type == IPC_CFG_STR)
            snprintf(line, sizeof(line), "cfg %-20s = \"%s\"", e->key, e->s);
        else
            snprintf(line, sizeof(line), "cfg %-20s = %ld%s", e->key, (long)e->i,
                     e->type == IPC_CFG_BOOL ? " (bool)" : "");
        print(line);
    }
    snprintf(line, sizeof(line), "cfg: %u khoa, %s", (unsigned)ipc_cfg_count(),
             s_dirty ? "CHUA LUU" : "da luu");
    print(line);
}
