/*
 * ipc_config.h - dich vu cau hinh: doc/ghi du lieu ra file.
 *
 * Phan tang ro rang:
 *   - Core (ipc_config.c) : giu bang key/value, tuan tu hoa, checksum,
 *                           gop nhieu lan ghi lam mot (debounce).
 *   - Storage backend     : chi biet chuyen byte vao/ra. File stdio dung
 *                           duoc cho ca desktop lan SPIFFS/FATFS tren MCU;
 *                           ban RAM dung cho test.
 *
 * Nho tach nhu vay, test tren desktop khong dong vao o dia that ma van chay
 * dung code tuan tu hoa + checksum se chay tren MCU.
 *
 * An toan mat dien: file duoc ghi ra ban tam roi moi doi ten de trong file
 * that. Dang ghi ma mat dien thi file cu van con nguyen ven.
 */
#ifndef IPC_CONFIG_H
#define IPC_CONFIG_H

#include "ipc_looper.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IPC_CFG_MAX_ENTRIES
#define IPC_CFG_MAX_ENTRIES 32
#endif

#ifndef IPC_CFG_KEY_LEN
#define IPC_CFG_KEY_LEN 24
#endif

#ifndef IPC_CFG_STR_LEN
#define IPC_CFG_STR_LEN 48
#endif

/* Bo dem tuan tu hoa. Phai du chua toan bo cau hinh dang van ban. */
#ifndef IPC_CFG_BUF_SIZE
#define IPC_CFG_BUF_SIZE 2048
#endif

typedef enum {
    IPC_CFG_OK = 0,
    IPC_CFG_ERR_ARG = -1,
    IPC_CFG_ERR_FULL = -2,
    IPC_CFG_ERR_NOT_FOUND = -3,
    IPC_CFG_ERR_TYPE = -4,
    IPC_CFG_ERR_IO = -5,
    IPC_CFG_ERR_CORRUPT = -6,   /* checksum sai -> giu nguyen gia tri mac dinh */
    IPC_CFG_ERR_NO_SPACE = -7,  /* bo dem tuan tu hoa khong du */
} ipc_cfg_err_t;

typedef enum {
    IPC_CFG_INT = 0,
    IPC_CFG_BOOL,
    IPC_CFG_STR,
} ipc_cfg_type_t;

/* ---------------- storage backend ---------------- */

typedef struct ipc_cfg_storage {
    const char *name;
    /* Doc toan bo vao buf. Tra so byte doc duoc, 0 neu chua co du lieu,
     * so am neu loi. */
    int (*load)(struct ipc_cfg_storage *self, char *buf, uint32_t cap);
    /* Ghi de toan bo. Tra 0 neu thanh cong. Phai la thao tac nguyen to
     * (ghi ban tam roi doi ten). */
    int (*save)(struct ipc_cfg_storage *self, const char *buf, uint32_t len);
    void *impl;
} ipc_cfg_storage_t;

/* Backend file (stdio). Dung cho desktop va cho SPIFFS/FATFS tren MCU.
 * path phai song lau bang chuong trinh (string literal la du). */
ipc_cfg_storage_t *ipc_cfg_storage_file(const char *path);

/* Backend RAM cho test: khong cham o dia, kiem tra duoc noi dung da ghi. */
typedef struct {
    ipc_cfg_storage_t base;
    char     buf[IPC_CFG_BUF_SIZE];
    uint32_t len;
    uint32_t save_count;
    uint32_t load_count;
    bool     fail_next_save;   /* bom loi IO de test duong that bai */
} ipc_cfg_ram_storage_t;

void ipc_cfg_storage_ram_init(ipc_cfg_ram_storage_t *rs);

/* ---------------- schema ---------------- */

/* Khai bao truoc cac khoa va gia tri mac dinh. Khoa la ban hop dong: file
 * hong hay thieu khoa thi ta ve mac dinh chu khong chay voi rac. */
typedef struct {
    const char    *key;
    ipc_cfg_type_t type;
    int32_t        def_int;     /* dung cho INT va BOOL */
    const char    *def_str;
} ipc_cfg_schema_t;

/* Duoc goi moi khi mot khoa doi gia tri (tren context nguoi goi set). */
typedef void (*ipc_cfg_change_fn)(const char *key, void *user);

typedef struct {
    ipc_cfg_storage_t *storage;
    const ipc_cfg_schema_t *schema;
    uint32_t schema_count;
    ipc_cfg_change_fn on_change;
    void *user;

    /*
     * Gop nhieu lan set lien tiep thanh MOT lan ghi, sau khoang lang nay.
     * 0 = ghi ngay khi set (khong khuyen khich: bao mon flash).
     * Can ipc_timer engine dang chay.
     */
    uint32_t autosave_delay_ms;
    /*
     * Looper thuc hien viec ghi. Nen dat: ghi file co the cham hang tram ms,
     * dat NULL se ghi ngay tren task timer va lam tre moi timer khac.
     */
    ipc_looper_t *writer_looper;
} ipc_cfg_cfg_t;

void ipc_cfg_cfg_default(ipc_cfg_cfg_t *cfg);

/* Nap schema, dat moi khoa ve mac dinh, roi doc tu storage de.
 * Tra IPC_CFG_ERR_CORRUPT neu du lieu hong (van chay duoc voi mac dinh). */
ipc_cfg_err_t ipc_cfg_init(const ipc_cfg_cfg_t *cfg);

ipc_cfg_err_t ipc_cfg_load(void);
ipc_cfg_err_t ipc_cfg_save(void);          /* ghi ngay, dong bo */
void          ipc_cfg_save_deferred(void); /* hen gio ghi, gop nhieu lan set */
bool          ipc_cfg_is_dirty(void);
void          ipc_cfg_reset_defaults(void);

/* ---------------- doc/ghi ---------------- */

int32_t     ipc_cfg_get_int(const char *key, int32_t fallback);
bool        ipc_cfg_get_bool(const char *key, bool fallback);
/* Chep gia tri vao out. Tra do dai chep duoc, hoac so am neu loi. */
int         ipc_cfg_get_str(const char *key, char *out, uint32_t cap,
                            const char *fallback);

ipc_cfg_err_t ipc_cfg_set_int(const char *key, int32_t v);
ipc_cfg_err_t ipc_cfg_set_bool(const char *key, bool v);
ipc_cfg_err_t ipc_cfg_set_str(const char *key, const char *v);

bool ipc_cfg_has(const char *key);
uint32_t ipc_cfg_count(void);
void ipc_cfg_dump(void (*print)(const char *line));

#ifdef __cplusplus
}
#endif
#endif /* IPC_CONFIG_H */
