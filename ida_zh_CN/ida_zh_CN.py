# -*- coding: utf-8 -*-
r"""IDA Pro 界面汉化补丁 —— 加载器(IDAPython 用户插件)。

启动时通过 ctypes 装载 ida_zh_CN\zh_hook.dll 安装文本钩子, 并用 PySide6
遍历当前所有窗口/菜单树: 未翻译文本枚举到 ida_zh_CN\ui_inventory.tsv,
已在词典中的就地翻译(解决 Quick Start 等先于钩子创建的界面)。
本文件必须位于 plugins 根目录; 资源在 plugins\ida_zh_CN\ 子目录。
run 参数: 0=重载词典+遍历 1=统计 2=关采集 3=开采集 4=仅遍历
"""
import os
import re
import ctypes
import idaapi

PLUGDIR = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(PLUGDIR, "ida_zh_CN")
DLL = os.path.join(RES, "zh_hook.dll")
DICT = os.path.join(RES, "zh_CN.tsv")
INVENTORY = os.path.join(RES, "ui_inventory.tsv")

_dll = None


def _install():
    global _dll
    if not os.path.isfile(DLL):
        print("[ida_zh_CN] zh_hook.dll 不存在: %s" % DLL)
        return
    try:
        _dll = ctypes.WinDLL(DLL)
        _dll.zhook_install.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
        _dll.zhook_install.restype = ctypes.c_int
        rc = _dll.zhook_install(DICT, RES)
        if rc == 0:
            print("[ida_zh_CN] 汉化钩子已安装 (%s)" % time_str())
        elif rc > 0:
            print("[ida_zh_CN] 已安装, %d 个钩子点未解析 (详见 ida_zh_CN\\zh_hook.log)" % rc)
        else:
            print("[ida_zh_CN] 安装失败 code=%d (详见 ida_zh_CN\\zh_hook.log)" % rc)
    except OSError as e:
        print("[ida_zh_CN] 加载失败:", e)


def time_str():
    import datetime
    return datetime.datetime.now().strftime("%H:%M:%S")


def _load_dict():
    d = {}
    try:
        with open(DICT, "r", encoding="utf-8-sig") as f:
            for line in f:
                line = line.rstrip("\n").rstrip("\r")
                if not line or line.startswith("#") or "\t" not in line:
                    continue
                k, v = line.split("\t", 1)
                k = k.rstrip(" ")
                if k and v.strip():
                    d[k] = v.strip()
    except OSError:
        pass
    return d


_ENT = {"&amp;": "&", "&lt;": "<", "&gt;": ">", "&quot;": '"', "&#39;": "'", "&apos;": "'"}


def _tr_node(d, s):
    s2 = _ENT.get(s, s)
    if s2 in d:
        return d[s2]
    r = d.get(s2.rstrip(" "))
    return r


def _tr_html(d, html):
    """翻译 HTML 文本节点, 标签原样保留。"""
    def sub(m):
        inner = m.group(1)
        if not inner.strip():
            return m.group(0)
        r = _tr_node(d, inner.strip())
        if r is None:
            return m.group(0)
        lead = inner[:len(inner) - len(inner.lstrip())]
        trail = inner[len(inner.rstrip()):]
        return ">" + lead + r + trail + "<"
    out = re.sub(r">([^<>]*)<", sub, html)
    m = re.match(r"^([^<]+)", out)
    if m:
        head = m.group(1)
        r = _tr_node(d, head.strip())
        if r is not None:
            out = r + out[len(head):]
    return out


def _tr_plain(d, t):
    if t in d:
        return d[t]
    r = d.get(t.rstrip(" "))
    if r is not None:
        return r
    return None


def _sweep():
    """遍历全部窗口/菜单树: 就地翻译 + 未翻译文本落盘 ui_inventory.tsv。"""
    try:
        from PySide6 import QtWidgets
    except ImportError:
        return 0
    app = QtWidgets.QApplication.instance()
    if app is None:
        return 0
    d = _load_dict()
    rows = []
    seen = set()

    def rec(kind, path, t):
        t = t.replace("\n", "\\n").replace("\r", "")
        if len(t) > 400:
            t = t[:400] + "..."
        key = (kind, t)
        if key not in seen and t.strip():
            seen.add(key)
            rows.append(kind + "|" + path + "\t" + t)

    def walk_action(act, path):
        try:
            t = act.text()
            if t:
                rec("act", path, t)
                nt = _tr_plain(d, t)
                if nt and nt != t:
                    act.setText(nt)
            tip = act.toolTip()
            if tip:
                rec("tip", path, tip)
                nt = _tr_plain(d, tip.replace("\n", " "))
                if nt and nt != tip:
                    act.setToolTip(nt)
            st = act.statusTip()
            if st:
                nt = _tr_plain(d, st)
                if nt and nt != st:
                    act.setStatusTip(nt)
            sub = act.menu()
            if sub is not None:
                p2 = path + "/" + (t or "?")
                for a2 in sub.actions():
                    walk_action(a2, p2)
        except Exception:
            pass

    n_tr = 0
    for w in app.allWidgets():
        try:
            cls = w.metaObject().className() if w.metaObject() else "?"
            for act in w.actions():
                walk_action(act, cls)
            ttl = w.windowTitle()
            if ttl:
                rec("title", cls, ttl)
                nt = _tr_plain(d, ttl)
                if nt and nt != ttl:
                    w.setWindowTitle(nt)
                    n_tr += 1
            if isinstance(w, QtWidgets.QLabel):
                t = w.text()
                if t:
                    rec("label", cls, t)
                    if "<" in t:
                        nt = _tr_html(d, t)
                    else:
                        nt = _tr_plain(d, t)
                    if nt and nt != t:
                        w.setText(nt)
                        n_tr += 1
            elif isinstance(w, QtWidgets.QAbstractButton):
                t = w.text()
                if t:
                    rec("btn", cls, t)
                    nt = _tr_plain(d, t)
                    if nt and nt != t:
                        w.setText(nt)
                        n_tr += 1
            elif isinstance(w, QtWidgets.QTextEdit) and w.readOnly():
                html = w.toHtml()
                if html:
                    rec("html", cls, html)
                    nt = _tr_html(d, html)
                    if nt != html:
                        w.setHtml(nt)
                        n_tr += 1
        except Exception:
            pass
    for w in app.topLevelWidgets():
        try:
            w.update()
        except Exception:
            pass
    if rows:
        try:
            with open(INVENTORY, "a", encoding="utf-8") as f:
                f.write("\n".join(rows) + "\n")
        except OSError:
            pass
    return n_tr


def _later():
    try:
        _sweep()
    except Exception:
        pass
    return -1  # 一次性定时器


class zh_plugin_t(idaapi.plugin_t):
    flags = getattr(idaapi, "PLUGIN_FIX", 0)
    wanted_name = "IDA_zh_CN"
    wanted_hotkey = ""

    def init(self):
        _install()
        try:
            _sweep()
            idaapi.register_timer(2500, _later)
            idaapi.register_timer(9000, _later)
        except Exception:
            pass
        return idaapi.PLUGIN_KEEP

    def run(self, arg):
        if arg == 0:
            if _dll:
                _dll.zhook_reload.restype = ctypes.c_int
                n = _dll.zhook_reload()
                print("[ida_zh_CN] 词典已重载: %d 条" % n)
            try:
                n = _sweep()
                print("[ida_zh_CN] 遍历完成, 就地翻译 %d 处" % n)
            except Exception as e:
                print("[ida_zh_CN] 遍历失败:", e)
        elif arg == 1:
            if _dll:
                _dll.zhook_stats()
                print("[ida_zh_CN] 统计已写入 ida_zh_CN\\zh_hook.log")
        elif arg == 2:
            if _dll:
                _dll.zhook_capture(0)
                print("[ida_zh_CN] 采集模式已关闭")
        elif arg == 3:
            if _dll:
                _dll.zhook_capture(1)
                print("[ida_zh_CN] 采集模式已开启")
        else:
            try:
                n = _sweep()
                print("[ida_zh_CN] 遍历完成, 就地翻译 %d 处" % n)
            except Exception as e:
                print("[ida_zh_CN] 遍历失败:", e)

    def term(self):
        pass


def PLUGIN_ENTRY():
    return zh_plugin_t()
