/*
 * zh_hook.c — IDA Pro 9.5 界面汉化钩子 (x64, C)
 *
 * 机制: inline hook Qt6 文本绘制/测量入口与 uxtheme 主题文本入口,
 *       查词典做精确匹配替换, 未命中原样放行。不修改任何文件,
 *       不触碰许可逻辑; 卸载 = 删除插件目录。
 *
 * 构建见 build_vs.bat (VS 2022) 。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <uchar.h>
#include <stdarg.h>
#include "MinHook.h"

#ifdef _MSC_VER
#  define EXPORT __declspec(dllexport)
#else
#  define EXPORT __attribute__((dllexport))
#endif

/* ================= Qt6 QString =================
 * Qt 6.x 发行版布局: QString{ QArrayDataPointer<char16_t>{ Data* d; char16_t* ptr; qsizetype size; } }
 * 运行时以合理性校验兜底; 校验失败即放弃替换(放行原文)。 */
typedef struct QStr { void* d; const char16_t* ptr; int64_t size; } QStr;

/* ================= 原始函数指针 (x64 统一调用约定) ================= */
typedef void    (*fn_dt_pf)(void*, const void*, const QStr*);             /* QPainter::drawText(QPointF&,QString&) */
typedef void    (*fn_dt_rf)(void*, const void*, int, const QStr*, void*); /* drawText(QRectF&,int,QString&,QRectF*) */
typedef void    (*fn_dt_r )(void*, const void*, int, const QStr*, void*); /* drawText(QRect&,int,QString&,QRect*) */
typedef void    (*fn_dt_xy)(void*, int, int, const QStr*);                /* drawText(int,int,QString&) */
typedef void    (*fn_dt_op)(void*, const void*, const QStr*, const void*);/* drawText(QRectF&,QString&,QTextOption&) */
typedef int     (*fn_ha_i )(const void*, const QStr*, int);               /* QFontMetrics::horizontalAdvance(QString,int) */
typedef float   (*fn_ha_f )(const void*, const QStr*, int);               /* QFontMetricsF::horizontalAdvance(QString,int) */
typedef void    (*fn_swt  )(void*, const QStr*);                          /* QWidget::setWindowTitle(QString&) 等 (self,QString&) 形态通用 */
typedef HRESULT (*fn_dtt  )(void*, void*, int, int, const wchar_t*, int, DWORD, DWORD, const RECT*);
typedef HRESULT (*fn_dttx )(void*, void*, int, int, const wchar_t*, int, DWORD, RECT*, const void*);
typedef void*   (*fn_fromWC)(void* ret, const wchar_t*, int64_t);         /* QString::fromWCharArray (静态, sret) */
typedef void    (*fn_dtor )(void*);

static fn_dt_pf  o_dt_pf; static fn_dt_rf o_dt_rf; static fn_dt_r o_dt_r;
static fn_dt_xy  o_dt_xy; static fn_dt_op o_dt_op;
static fn_ha_i   o_ha_i;  static fn_ha_f  o_ha_f;
static fn_swt    o_swt;
static fn_swt    o_act_text, o_act_tip, o_act_stip;    /* QAction::setText/setToolTip/setStatusTip (Qt6Gui) */
static fn_swt    o_label, o_btn, o_wtip;               /* QLabel/QAbstractButton::setText, QWidget::setToolTip */
static fn_swt    o_setpt;                              /* QTextDocument::setPlainText */
static fn_swt    o_sethtml;                            /* QTextDocument::setHtml */
static fn_dtt    o_dtt;   static fn_dttx  o_dttx;
static fn_fromWC q_fromWC; static fn_dtor  q_dtor;

/* ================= 状态 ================= */
static CRITICAL_SECTION g_cs;
static volatile LONG g_capture = 1;          /* 采集模式: 未命中字符串落盘 */
static __declspec(thread) int g_depth;       /* 重入保护 */
static __declspec(thread) QStr g_tmp;        /* 本线程替换用临时 QString(24 字节) */

static wchar_t g_dbgpath[MAX_PATH] = L"";
static wchar_t g_cappath[MAX_PATH] = L"";
static wchar_t g_dictpath[MAX_PATH] = L"";

enum { H_DT_PF = 0, H_DT_RF, H_DT_R, H_DT_XY, H_DT_OP, H_HA_I, H_HA_F, H_SWT, H_DTT, H_DTTX,
       H_ACT, H_ACT_TIP, H_ACT_STIP, H_LABEL, H_BTN, H_WTIP, H_SETP, H_SETHTML, H_COUNT };
static volatile LONG g_capn[H_COUNT];
static const char* g_hname[H_COUNT] = {
    "drawText(QPointF,QString)", "drawText(QRectF,int,QString)", "drawText(QRect,int,QString)",
    "drawText(x,y,QString)", "drawText(QRectF,QString,TextOption)",
    "hAdvance(int)", "hAdvanceF(int)", "setWindowTitle", "DrawThemeText", "DrawThemeTextEx",
    "QAction::setText", "QAction::setToolTip", "QAction::setStatusTip",
    "QLabel::setText", "QAbstractButton::setText", "QWidget::setToolTip",
    "QTextDocument::setPlainText", "QTextDocument::setHtml"
};

/* ================= 调试日志 ================= */
static void dbg_line(const char* fmt, ...) {
    FILE* f; va_list ap;
    if (!g_dbgpath[0]) return;
    f = _wfopen(g_dbgpath, L"ab");
    if (!f) return;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

/* ================= 词典: 开放定址哈希 (FNV-1a) ================= */
typedef struct { wchar_t* k; int klen; wchar_t* v; int vlen; } Ent;
static Ent*    g_tab;
static size_t  g_tcap, g_tcnt;

static uint64_t hh(const wchar_t* s, int n) {
    uint64_t h = 1469598103934665603ULL; int i;
    for (i = 0; i < n; i++) { h ^= (uint16_t)s[i]; h *= 1099511628211ULL; }
    return h;
}
static void tab_put(const wchar_t* k, int klen, const wchar_t* v, int vlen) {
    uint64_t h; size_t m, i;
    if (!k || klen <= 0 || vlen <= 0) return;
    if (!g_tcap || (g_tcnt + 1) * 10 >= g_tcap * 7) { /* 扩容 */
        size_t ncap = g_tcap ? g_tcap * 2 : 8192; Ent* nt = (Ent*)calloc(ncap, sizeof(Ent)); size_t j;
        if (!nt) return;
        for (i = 0; i < g_tcap; i++) if (g_tab && g_tab[i].k) {
            h = hh(g_tab[i].k, g_tab[i].klen); j = h & (ncap - 1);
            while (nt[j].k) j = (j + 1) & (ncap - 1);
            nt[j] = g_tab[i];
        }
        free(g_tab); g_tab = nt; g_tcap = ncap;
    }
    h = hh(k, klen); m = g_tcap - 1; i = h & m;
    while (g_tab[i].k) {
        if (g_tab[i].klen == klen && memcmp(g_tab[i].k, k, (size_t)klen * sizeof(wchar_t)) == 0) {
            wchar_t* nv = (wchar_t*)malloc(((size_t)vlen + 1) * sizeof(wchar_t));
            if (nv) { memcpy(nv, v, (size_t)vlen * sizeof(wchar_t)); nv[vlen] = 0; free(g_tab[i].v); g_tab[i].v = nv; g_tab[i].vlen = vlen; }
            return;
        }
        i = (i + 1) & m;
    }
    {
        wchar_t* nk = (wchar_t*)malloc(((size_t)klen + 1) * sizeof(wchar_t));
        wchar_t* nv = (wchar_t*)malloc(((size_t)vlen + 1) * sizeof(wchar_t));
        if (!nk || !nv) { free(nk); free(nv); return; }
        memcpy(nk, k, (size_t)klen * sizeof(wchar_t)); nk[klen] = 0;
        memcpy(nv, v, (size_t)vlen * sizeof(wchar_t)); nv[vlen] = 0;
        g_tab[i].k = nk; g_tab[i].klen = klen; g_tab[i].v = nv; g_tab[i].vlen = vlen; g_tcnt++;
    }
}
static const Ent* tab_get(const wchar_t* k, int klen) {
    uint64_t h; size_t m, i;
    if (!g_tcap || !k || klen <= 0) return NULL;
    h = hh(k, klen); m = g_tcap - 1; i = h & m;
    while (g_tab[i].k) {
        if (g_tab[i].klen == klen && memcmp(g_tab[i].k, k, (size_t)klen * sizeof(wchar_t)) == 0) return &g_tab[i];
        i = (i + 1) & m;
    }
    return NULL;
}
static void tab_clear(void) {
    size_t i;
    for (i = 0; i < g_tcap; i++) { if (g_tab && g_tab[i].k) { free(g_tab[i].k); free(g_tab[i].v); } }
    free(g_tab); g_tab = NULL; g_tcap = 0; g_tcnt = 0;
}

/* 词典文件: UTF-8 TSV, "原文\t译文", '#' 开头为注释 */
static void dict_load(const wchar_t* path) {
    FILE* f; long n; unsigned char* buf; size_t rd; long off = 0;
    int wlen; wchar_t* w; wchar_t *p, *end;
    f = _wfopen(path, L"rb");
    if (!f) { dbg_line("dict: cannot open"); return; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (64L << 20)) { fclose(f); return; }
    buf = (unsigned char*)malloc((size_t)n);
    if (!buf) { fclose(f); return; }
    rd = fread(buf, 1, (size_t)n, f); fclose(f);
    if (rd >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) off = 3;
    wlen = MultiByteToWideChar(CP_UTF8, 0, (const char*)buf + off, (int)(rd - off), NULL, 0);
    if (wlen <= 0) { free(buf); return; }
    w = (wchar_t*)malloc(((size_t)wlen + 1) * sizeof(wchar_t));
    if (!w) { free(buf); return; }
    MultiByteToWideChar(CP_UTF8, 0, (const char*)buf + off, (int)(rd - off), w, wlen); w[wlen] = 0;
    p = w; end = w + wlen;
    while (p < end) {
        wchar_t* nl  = wcschr(p, L'\n');
        wchar_t* lend = nl ? nl : end;
        wchar_t* tab = wcschr(p, L'\t');
        if (tab && tab < lend && p[0] != L'#') {
            wchar_t* k = p;    int klen = (int)(tab - p);
            wchar_t* v = tab + 1; int vlen = (int)(lend - v);
            while (klen > 0 && (k[klen - 1] == L'\r' || k[klen - 1] == L' ')) klen--;
            while (vlen > 0 && (v[vlen - 1] == L'\r' || v[vlen - 1] == L' ')) vlen--;
            while (*k == L' ') { k++; klen--; }
            while (*v == L' ') { v++; vlen--; }
            if (klen > 0 && vlen > 0) tab_put(k, klen, v, vlen);
        }
        p = nl ? nl + 1 : end;
    }
    free(w); free(buf);
}

/* ================= 采集(未命中 -> captured.tsv) ================= */
#define CAPMAX 32768
typedef struct { wchar_t* s; int n; uint64_t h; } CapE;
static CapE*   g_caps;
static size_t  g_capsz = 65536, g_capcnt;

static int plausible(const char16_t* s, int n) {
    int letters = 0, digits = 0, i, spaces = 0;
    if (n < 2 || n > 120) return 0;
    for (i = 0; i < n; i++) {
        unsigned c = s[i];
        if (c == '\t' || c == '\n' || c == '\r') return 0;
        if (c == ' ') { if (++spaces >= 2) return 0; continue; } /* 反汇编行靠连续空格对齐, 直接排除 */
        spaces = 0;
        if (c < 0x80) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) letters++;
            else if (c >= '0' && c <= '9') digits++;
            else if (strchr(" .,:;!?'\"()[]{}<>-_/\\%+=|*#@$~^&", (int)c)) ;
            else return 0;
        } else if (c == 0x2026 || c == 0x00B7 || (c >= 0x2018 && c <= 0x201D)) letters++;
        else return 0;
    }
    return letters >= 3 && digits <= 3;
}

static void cap_add(const char16_t* s, int n, int hook) {
    uint64_t h; size_t i;
    if (!g_capture || !s || n < 2 || n > 120 || !plausible(s, n)) return;
    h = hh((const wchar_t*)s, n);
    EnterCriticalSection(&g_cs);
    if (!g_caps) g_caps = (CapE*)calloc(g_capsz, sizeof(CapE));
    if (!g_caps) { LeaveCriticalSection(&g_cs); return; }
    /* 开放定址查重 */
    i = (size_t)h & (g_capsz - 1);
    while (g_caps[i].s) {
        if (g_caps[i].h == h && g_caps[i].n == n && memcmp(g_caps[i].s, s, (size_t)n * sizeof(char16_t)) == 0) {
            LeaveCriticalSection(&g_cs); return;
        }
        i = (i + 1) & (g_capsz - 1);
    }
    if (g_capcnt < CAPMAX) {
        wchar_t* cp = (wchar_t*)malloc(((size_t)n + 1) * sizeof(wchar_t));
        if (cp) {
            FILE* f;
            memcpy(cp, s, (size_t)n * sizeof(char16_t)); cp[n] = 0;
            g_caps[i].s = cp; g_caps[i].n = n; g_caps[i].h = h; g_capcnt++;
            InterlockedIncrement(&g_capn[hook]);
            f = _wfopen(g_cappath, L"ab");
            if (f) {
                int u8n = WideCharToMultiByte(CP_UTF8, 0, cp, n, NULL, 0, NULL, NULL);
                if (u8n > 0) {
                    char* u8 = (char*)malloc((size_t)u8n);
                    if (u8) { WideCharToMultiByte(CP_UTF8, 0, cp, n, u8, u8n, NULL, NULL); fwrite(u8, 1, (size_t)u8n, f); free(u8); }
                }
                fwrite("\t\n", 1, 2, f);
                fclose(f);
            }
        }
    }
    LeaveCriticalSection(&g_cs);
}

/* ================= 替换辅助 ================= */
static int qstr_get(const QStr* q, const char16_t** p, int64_t* n) {
    if (!q || !q->ptr || q->size <= 0 || q->size > 0x8000) return 0;
    *p = q->ptr; *n = q->size;
    return 1;
}
static const Ent* rep_of(const QStr* s, int hook) {
    const char16_t* p; int64_t n; const Ent* e;
    if (g_depth || !qstr_get(s, &p, &n)) return NULL;
    e = tab_get((const wchar_t*)p, (int)n);
    if (!e) { /* 尾部空格容错: 菜单/状态栏文本常带填充空格 */
        int64_t m = n;
        while (m > 0 && p[m - 1] == ' ') m--;
        if (m > 0 && m < n) e = tab_get((const wchar_t*)p, (int)m);
    }
    if (!e) cap_add(p, (int)n, hook);
    return e;
}

/* ================= 钩子 ================= */
static void hk_dt_pf(void* self, const void* pt, const QStr* s) {
    const Ent* e = rep_of(s, H_DT_PF);
    if (e) { g_depth = 1; q_fromWC(&g_tmp, e->v, e->vlen); o_dt_pf(self, pt, &g_tmp); g_depth = 0; q_dtor(&g_tmp); return; }
    o_dt_pf(self, pt, s);
}
static void hk_dt_rf(void* self, const void* r, int fl, const QStr* s, void* br) {
    const Ent* e = rep_of(s, H_DT_RF);
    if (e) { g_depth = 1; q_fromWC(&g_tmp, e->v, e->vlen); o_dt_rf(self, r, fl, &g_tmp, br); g_depth = 0; q_dtor(&g_tmp); return; }
    o_dt_rf(self, r, fl, s, br);
}
static void hk_dt_r(void* self, const void* r, int fl, const QStr* s, void* br) {
    const Ent* e = rep_of(s, H_DT_R);
    if (e) { g_depth = 1; q_fromWC(&g_tmp, e->v, e->vlen); o_dt_r(self, r, fl, &g_tmp, br); g_depth = 0; q_dtor(&g_tmp); return; }
    o_dt_r(self, r, fl, s, br);
}
static void hk_dt_xy(void* self, int x, int y, const QStr* s) {
    const Ent* e = rep_of(s, H_DT_XY);
    if (e) { g_depth = 1; q_fromWC(&g_tmp, e->v, e->vlen); o_dt_xy(self, x, y, &g_tmp); g_depth = 0; q_dtor(&g_tmp); return; }
    o_dt_xy(self, x, y, s);
}
static void hk_dt_op(void* self, const void* r, const QStr* s, const void* op) {
    const Ent* e = rep_of(s, H_DT_OP);
    if (e) { g_depth = 1; q_fromWC(&g_tmp, e->v, e->vlen); o_dt_op(self, r, &g_tmp, op); g_depth = 0; q_dtor(&g_tmp); return; }
    o_dt_op(self, r, s, op);
}
static int hk_ha_i(const void* self, const QStr* s, int tab) {
    const Ent* e = rep_of(s, H_HA_I);
    if (e) { int r; g_depth = 1; q_fromWC(&g_tmp, e->v, e->vlen); r = o_ha_i(self, &g_tmp, tab); g_depth = 0; q_dtor(&g_tmp); return r; }
    return o_ha_i(self, s, tab);
}
static float hk_ha_f(const void* self, const QStr* s, int tab) {
    const Ent* e = rep_of(s, H_HA_F);
    if (e) { float r; g_depth = 1; q_fromWC(&g_tmp, e->v, e->vlen); r = o_ha_f(self, &g_tmp, tab); g_depth = 0; q_dtor(&g_tmp); return r; }
    return o_ha_f(self, s, tab);
}
static void hk_swt(void* self, const QStr* s) {
    const Ent* e = rep_of(s, H_SWT);
    if (e) { g_depth = 1; q_fromWC(&g_tmp, e->v, e->vlen); o_swt(self, &g_tmp); g_depth = 0; q_dtor(&g_tmp); return; }
    o_swt(self, s);
}

/* ---- 源头级 hook: (self, const QString&) 形态统一生成 ---- */
#define TEXT_HOOK(NAME, ORIG, HOOKID) \
static void hk_##NAME(void* self, const QStr* s) { \
    const Ent* e = rep_of(s, HOOKID); \
    if (e) { g_depth = 1; q_fromWC(&g_tmp, e->v, e->vlen); ORIG(self, &g_tmp); g_depth = 0; q_dtor(&g_tmp); return; } \
    ORIG(self, s); \
}
TEXT_HOOK(act_text, o_act_text, H_ACT)
TEXT_HOOK(act_tip,  o_act_tip,  H_ACT_TIP)
TEXT_HOOK(act_stip, o_act_stip, H_ACT_STIP)
TEXT_HOOK(label,    o_label,    H_LABEL)
TEXT_HOOK(btn,      o_btn,      H_BTN)
TEXT_HOOK(wtip,     o_wtip,     H_WTIP)
TEXT_HOOK(setpt,    o_setpt,    H_SETP)

/* ---- QTextDocument::setHtml: 只翻译标签之间的文本节点(HTML 感知) ---- */
static int node_lookup(const char16_t* tp, int64_t tn, const Ent** out) {
    const Ent* e;
    while (tn > 0 && (tp[0] == ' ' || tp[0] == '\n' || tp[0] == '\r' || tp[0] == '\t')) { tp++; tn--; }
    while (tn > 0 && (tp[tn-1] == ' ' || tp[tn-1] == '\n' || tp[tn-1] == '\r' || tp[tn-1] == '\t')) tn--;
    if (tn < 2) return 0;
    e = tab_get((const wchar_t*)tp, (int)tn);
    if (!e && tn < 300) { /* 常见实体解码后再查一次 */
        wchar_t dec[300]; int dn = 0, i;
        for (i = 0; i < (int)tn && dn < 299; i++) {
            if (tp[i] == '&') {
                if (tn - i >= 5 && tp[i+1]=='a' && tp[i+2]=='m' && tp[i+3]=='p' && tp[i+4]==';') { dec[dn++] = '&'; i += 4; continue; }
                if (tn - i >= 4 && tp[i+1]=='l' && tp[i+2]=='t' && tp[i+3]==';') { dec[dn++] = '<'; i += 3; continue; }
                if (tn - i >= 4 && tp[i+1]=='g' && tp[i+2]=='t' && tp[i+3]==';') { dec[dn++] = '>'; i += 3; continue; }
                if (tn - i >= 6 && tp[i+1]=='q' && tp[i+2]=='u' && tp[i+3]=='o' && tp[i+4]=='t' && tp[i+5]==';') { dec[dn++] = '"'; i += 5; continue; }
                if (tn - i >= 6 && tp[i+1]=='#' && tp[i+2]=='3' && tp[i+3]=='9' && tp[i+4]==';') { dec[dn++] = '\''; i += 5; continue; }
            }
            dec[dn++] = (wchar_t)tp[i];
        }
        e = tab_get(dec, dn);
    }
    if (!e) return 0;
    *out = e;
    return 1;
}
static void hk_sethtml(void* self, const QStr* s) {
    const char16_t* p; int64_t n;
    if (!g_depth && g_tcnt && qstr_get(s, &p, &n) && n > 8 && n < (256 << 10)) {
        /* pass1: 是否存在可翻译文本节点 */
        int64_t i = 0; int hit = 0;
        while (i < n && !hit) {
            int64_t lt = i, gt;
            const Ent* e;
            while (lt < n && p[lt] != '<') lt++;
            if (node_lookup(p + i, lt - i, &e)) hit = 1;
            if (lt >= n) break;
            gt = lt;
            while (gt < n && p[gt] != '>') gt++;
            i = (gt < n) ? gt + 1 : n;
        }
        if (hit) {
            wchar_t* out = (wchar_t*)malloc(((size_t)n * 2 + 16) * sizeof(wchar_t));
            if (out) {
                int64_t i2 = 0; size_t o2 = 0;
                while (i2 < n) { /* pass2: 逐节点重建 */
                    int64_t lt = i2, gt, k;
                    const Ent* e = NULL;
                    const char16_t* tp;
                    int64_t tn, lead = 0, core;
                    while (lt < n && p[lt] != '<') lt++;
                    tp = p + i2; tn = lt - i2;
                    while (lead < tn && (tp[lead] == ' ' || tp[lead] == '\n' || tp[lead] == '\r' || tp[lead] == '\t')) lead++;
                    core = tn;
                    while (core > lead && (tp[core-1] == ' ' || tp[core-1] == '\n' || tp[core-1] == '\r' || tp[core-1] == '\t')) core--;
                    if (node_lookup(tp, tn, &e)) {
                        for (k = 0; k < lead; k++) out[o2++] = (wchar_t)tp[k];
                        for (k = 0; k < e->vlen; k++) out[o2++] = e->v[k];
                        for (k = core; k < tn; k++) out[o2++] = (wchar_t)tp[k];
                    } else {
                        for (k = 0; k < tn; k++) out[o2++] = (wchar_t)tp[k];
                    }
                    if (lt < n) {
                        gt = lt;
                        while (gt < n && p[gt] != '>') gt++;
                        if (gt < n) gt++;
                        for (k = lt; k < gt; k++) out[o2++] = (wchar_t)p[k];
                        i2 = gt;
                    } else i2 = n;
                }
                out[o2] = 0;
                g_depth = 1;
                q_fromWC(&g_tmp, out, (int64_t)o2);
                o_sethtml(self, &g_tmp);
                g_depth = 0;
                q_dtor(&g_tmp);
                free(out);
                return;
            }
        }
    }
    o_sethtml(self, s);
}
static HRESULT hk_dtt(void* th, void* dc, int part, int st, const wchar_t* tx, int cch, DWORD f1, DWORD f2, const RECT* rc) {
    if (tx && cch != 0) {
        int len = cch >= 0 ? cch : (int)wcslen(tx);
        const Ent* e = tab_get(tx, len);
        if (e) return o_dtt(th, dc, part, st, e->v, e->vlen, f1, f2, rc);
        cap_add((const char16_t*)tx, len, H_DTT);
    }
    return o_dtt(th, dc, part, st, tx, cch, f1, f2, rc);
}
static HRESULT hk_dttx(void* th, void* dc, int part, int st, const wchar_t* tx, int cch, DWORD f1, RECT* rc, const void* op) {
    if (tx && cch != 0) {
        int len = cch >= 0 ? cch : (int)wcslen(tx);
        const Ent* e = tab_get(tx, len);
        if (e) return o_dttx(th, dc, part, st, e->v, e->vlen, f1, rc, op);
        cap_add((const char16_t*)tx, len, H_DTTX);
    }
    return o_dttx(th, dc, part, st, tx, cch, f1, rc, op);
}

/* ================= 安装 ================= */
static int hook_one(HMODULE m, const char* sym, void* detour, void** orig) {
    void* p = (void*)GetProcAddress(m, sym);
    MH_STATUS st;
    if (!p) { dbg_line("MISS  %s", sym); return 1; }
    st = MH_CreateHook(p, detour, orig);
    if (st != MH_OK) { dbg_line("CFAIL %s (MH %d)", sym, (int)st); return 1; }
    dbg_line("HOOK  %s", sym);
    return 0;
}

EXPORT int __stdcall zhook_install(const wchar_t* dict_path, const wchar_t* log_dir) {
    HMODULE hcore, hgui, hwid, htheme;
    int fails = 0;
    InitializeCriticalSection(&g_cs);
    _snwprintf(g_dbgpath, MAX_PATH, L"%s\\zh_hook.log", log_dir);
    _snwprintf(g_cappath, MAX_PATH, L"%s\\captured.tsv", log_dir);
    _snwprintf(g_dictpath, MAX_PATH, L"%s", dict_path);
    DeleteFileW(g_dbgpath);
    dbg_line("== zh_hook install ==");
    hcore = GetModuleHandleW(L"Qt6Core.dll");
    hgui  = GetModuleHandleW(L"Qt6Gui.dll");
    hwid  = GetModuleHandleW(L"Qt6Widgets.dll");
    if (!hcore || !hgui) { dbg_line("Qt not loaded (core=%p gui=%p) -> idat/console?", (void*)hcore, (void*)hgui); return -1; }
    q_fromWC = (fn_fromWC)GetProcAddress(hcore, "?fromWCharArray@QString@QT@@SA?AV12@PEB_W_J@Z");
    q_dtor   = (fn_dtor)GetProcAddress(hcore, "??1QString@QT@@QEAA@XZ");
    if (!q_fromWC || !q_dtor) { dbg_line("QString helpers missing"); return -2; }
    if (MH_Initialize() != MH_OK) { dbg_line("MH_Initialize failed"); return -3; }

    fails += hook_one(hgui, "?drawText@QPainter@QT@@QEAAXAEBVQPointF@2@AEBVQString@2@@Z", hk_dt_pf, (void**)&o_dt_pf);
    fails += hook_one(hgui, "?drawText@QPainter@QT@@QEAAXAEBVQRectF@2@HAEBVQString@2@PEAV32@@Z", hk_dt_rf, (void**)&o_dt_rf);
    fails += hook_one(hgui, "?drawText@QPainter@QT@@QEAAXAEBVQRect@2@HAEBVQString@2@PEAV32@@Z", hk_dt_r, (void**)&o_dt_r);
    fails += hook_one(hgui, "?drawText@QPainter@QT@@QEAAXHHAEBVQString@2@@Z", hk_dt_xy, (void**)&o_dt_xy);
    fails += hook_one(hgui, "?drawText@QPainter@QT@@QEAAXAEBVQRectF@2@AEBVQString@2@AEBVQTextOption@2@@Z", hk_dt_op, (void**)&o_dt_op);
    fails += hook_one(hgui, "?horizontalAdvance@QFontMetrics@QT@@QEBAHAEBVQString@2@H@Z", hk_ha_i, (void**)&o_ha_i);
    fails += hook_one(hgui, "?horizontalAdvance@QFontMetricsF@QT@@QEBANAEBVQString@2@H@Z", hk_ha_f, (void**)&o_ha_f);
    if (hwid) fails += hook_one(hwid, "?setWindowTitle@QWidget@QT@@QEAAXAEBVQString@2@@Z", hk_swt, (void**)&o_swt);
    else dbg_line("Qt6Widgets not loaded");
    /* 源头级 hook */
    fails += hook_one(hgui, "?setText@QAction@QT@@QEAAXAEBVQString@2@@Z", hk_act_text, (void**)&o_act_text);
    fails += hook_one(hgui, "?setToolTip@QAction@QT@@QEAAXAEBVQString@2@@Z", hk_act_tip, (void**)&o_act_tip);
    fails += hook_one(hgui, "?setStatusTip@QAction@QT@@QEAAXAEBVQString@2@@Z", hk_act_stip, (void**)&o_act_stip);
    fails += hook_one(hgui, "?setPlainText@QTextDocument@QT@@QEAAXAEBVQString@2@@Z", hk_setpt, (void**)&o_setpt);
    fails += hook_one(hgui, "?setHtml@QTextDocument@QT@@QEAAXAEBVQString@2@@Z", hk_sethtml, (void**)&o_sethtml);
    if (hwid) {
        fails += hook_one(hwid, "?setText@QLabel@QT@@QEAAXAEBVQString@2@@Z", hk_label, (void**)&o_label);
        fails += hook_one(hwid, "?setText@QAbstractButton@QT@@QEAAXAEBVQString@2@@Z", hk_btn, (void**)&o_btn);
        fails += hook_one(hwid, "?setToolTip@QWidget@QT@@QEAAXAEBVQString@2@@Z", hk_wtip, (void**)&o_wtip);
    }
    htheme = GetModuleHandleW(L"uxtheme.dll");
    if (!htheme) htheme = LoadLibraryW(L"uxtheme.dll");
    if (htheme) {
        fails += hook_one(htheme, "DrawThemeText",   hk_dtt,  (void**)&o_dtt);
        fails += hook_one(htheme, "DrawThemeTextEx", hk_dttx, (void**)&o_dttx);
    } else dbg_line("uxtheme not available");

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { dbg_line("MH_EnableHook failed"); return -4; }
    dict_load(g_dictpath);
    dbg_line("dict entries: %zu, hook setup fails: %d", g_tcnt, fails);
    return fails;
}

EXPORT int __stdcall zhook_reload(void) {
    EnterCriticalSection(&g_cs);
    tab_clear();
    dict_load(g_dictpath);
    LeaveCriticalSection(&g_cs);
    dbg_line("dict reloaded: %zu entries", g_tcnt);
    return (int)g_tcnt;
}
EXPORT void __stdcall zhook_capture(int on) { g_capture = on ? 1 : 0; dbg_line("capture -> %d", on ? 1 : 0); }
EXPORT void __stdcall zhook_stats(void) {
    int i;
    dbg_line("== stats: dict=%zu capture=%ld ==", g_tcnt, g_capture);
    for (i = 0; i < H_COUNT; i++) dbg_line("  %-32s captured=%ld", g_hname[i], g_capn[i]);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
