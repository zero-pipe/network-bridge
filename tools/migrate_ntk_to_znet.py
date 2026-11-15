#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把代码树里对 ntk（network-tool-kit）的调用机械改名为 znet（幂等）。

3rdparty/znet 是 3rdparty/ntk 的 1:1 改名版，因此本脚本只做名字与 include 路径
的等价替换，不改任何语义：

    标识符      ntk_xxx          -> zn_xxx
                NTK_XXX          -> ZN_XXX
                ntk_xxx_t        -> zn_xxx_t
    include     <ntk.h>          -> <zn/zn.h>
                <ntk_errno.h>    -> <zn/zn_errno.h>
                <ntk/xxx.h>      -> <zn/xxx.h>
                <platform.h>     -> <zn/platform.h>
                <poller/...>     -> <zn/poller/...>
                <net/...>        -> <zn/net/...>
                <util/...>       -> <zn/util/...>
                <thread/thread_pool.h> -> <zn/thread/thread_pool.h>
    路径       3rdparty/ntk      -> 3rdparty/znet
    裸词        ntk              -> znet（含 CMake 目标名、注释与文档；libntk -> libznet）

例外：znet 把线程 / 互斥量 / 信号量下沉到了更底层的 zbase，这几个符号在 znet
里没有对应物，直接映射到 zbase 的 1:1 同名 API（znet 自身也包含 zb/os/*.h）：

    ntk_sem*        -> zb_sem*          （ntk/thread/sem.h   -> zb/os/sem.h）
    ntk_mutex*      -> zb_mutex*        （ntk/thread/sync.h  -> zb/os/sync.h）
    NTK_MUTEX_*     -> ZB_MUTEX_*
    ntk_thread*     -> zb_thread*       （ntk/thread/thread.h -> zb/os/thread.h）
    NTK_THREAD_PRIO_* -> ZB_THREAD_PRIO_*
    ntk_thread_pool* 仍属 znet：-> zn_thread_pool*

用法（在仓库任意位置执行均可，默认根目录为本脚本上一级）：

    # 迁移整棵树，成功且无残留后删除 3rdparty/ntk
    python tools/migrate_ntk_to_znet.py

    # 分步合并：先改代码，暂时保留 3rdparty/ntk 供对照
    python tools/migrate_ntk_to_znet.py --keep-ntk

    # 只看会改什么，不写盘
    python tools/migrate_ntk_to_znet.py --dry-run --verbose

    # CI 门禁：只检查是否还有残留（不改文件），有残留则退出码 1
    python tools/migrate_ntk_to_znet.py --check

    # 只迁移部分路径（可叠加），用于多 agent 并行改同一棵树
    python tools/migrate_ntk_to_znet.py --keep-ntk \
        --only src/rpc --only tests \
        --skip src/rpc/discovery --skip tests/test_dds_api.c

    # 其它开关
    --root DIR         指定仓库根目录
    --keep-ntk         不删除 3rdparty/ntk
    --keep-word-ntk    不替换裸露的 "ntk" 单词（只改标识符与 include）
    --force            有残留引用时也删除 3rdparty/ntk
    --dry-run          不写盘、不删目录
    --check            只检查残留
    --verbose          打印每个文件的改动条数

行为约定：
  * 幂等：已迁移的文件（zn_/ZN_/<zn/...>）不会被再次改动，可对任意代码树反复执行。
  * 不碰 3rdparty/：znet 与 zbase 自身源码永远不被改写；3rdparty/ntk 只由本脚本
    的整体迁移流程删除。
  * 删除保护：只要树里（3rdparty 之外）还残留 ntk 引用，就跳过删除并打印残留位置，
    除非显式 --force。删除守卫不看 --only/--skip：局部迁移不能让整棵树失去依赖。
  * 只有 ZNET 缺失的 handful 符号（见 GAP_SYMBOLS）无法机械映射，脚本仍按 1:1
    改名，并在结尾把它们列为"缺口清单"，需要人工补垫片或推动 znet 补齐。
"""

from __future__ import print_function

import argparse
import os
import re
import shutil
import sys

# ---------------------------------------------------------------------------
# 扫描范围
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TEXT_SUFFIXES = (
    ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".in",
    ".cmake", ".txt", ".md", ".py", ".sh", ".bat", ".ps1",
    ".json", ".yml", ".yaml", ".toml", ".cfg",
)

# 目录名：构建产物、缓存、vendored 代码都不参与改写。
SKIP_DIR_NAMES = frozenset((
    ".git", ".claude", ".cache", ".vs", ".vscode", ".idea",
    "__pycache__", "node_modules", "out", "Testing", "3rdparty",
))
SKIP_DIR_PREFIXES = ("build",)  # build/ build-dds/ build-xxx/ 等

# 本脚本自身充满 ntk 字面量（映射表、文档），永远不参与改写。
SELF_REL_PATH = "tools/migrate_ntk_to_znet.py"

# ---------------------------------------------------------------------------
# include 映射：ntk 头文件（相对 include 根）-> znet / zbase 头文件
# ---------------------------------------------------------------------------

# 全路径映射（名字变了，或者落到了 zbase）。
INCLUDE_MAP = {
    "ntk.h": "zn/zn.h",
    "ntk_errno.h": "zn/zn_errno.h",
    "ntk_export.h": "zn/zn_export.h",
    "platform.h": "zn/platform.h",
    # znet 把线程原语下沉到 zbase
    "thread/sem.h": "zb/os/sem.h",
    "thread/sync.h": "zb/os/sync.h",
    "thread/thread.h": "zb/os/thread.h",
    # 线程池仍是 znet 的一部分
    "thread/thread_pool.h": "zn/thread/thread_pool.h",
}

# 目录前缀映射：<dir>/xxx.h -> <新前缀>/xxx.h
INCLUDE_DIR_MAP = {
    "ntk": "zn",        # <ntk/xxx.h> -> <zn/xxx.h>
    "net": "zn/net",    # <net/socket.h> -> <zn/net/socket.h>
    "poller": "zn/poller",
    "util": "zn/util",
}

# ---------------------------------------------------------------------------
# 标识符映射：(正则, 替换) —— 顺序敏感，特例必须排在通用前缀之前
# ---------------------------------------------------------------------------

SYMBOL_RULES = (
    # C99 里 znet 已迁走/下沉的常量
    (r"\bNTK_MUTEX_(NORMAL|RECURSIVE)\b", r"ZB_MUTEX_\1"),
    (r"\bNTK_THREAD_PRIO_([A-Z]+)\b", r"ZB_THREAD_PRIO_\1"),
    # 互斥量 / 信号量 / 线程 -> zbase
    (r"\bntk_mutex\b", "zb_mutex"),
    (r"\bntk_mutex_", "zb_mutex_"),
    (r"\bntk_sem\b", "zb_sem"),
    (r"\bntk_sem_", "zb_sem_"),
    # 线程池例外：仍是 znet 的符号，必须先于 ntk_thread_ 处理
    (r"\bntk_thread_pool\b", "zn_thread_pool"),
    (r"\bntk_thread_pool_", "zn_thread_pool_"),
    (r"\bntk_thread_", "zb_thread_"),
    (r"\bntk_thread\b", "zb_thread"),
    # 通用 1:1 前缀
    (r"\bntk_", "zn_"),
    (r"\bNTK_", "ZN_"),
)

SYMBOL_RES = tuple((re.compile(p), r) for p, r in SYMBOL_RULES)

# 裸词与路径（最后处理，避免影响已改写的 include）。
WORD_RULES = (
    (re.compile(r"\blibntk\b"), "libznet"),
    (re.compile(r"3rdparty[/\\]ntk\b"), "3rdparty/znet"),
    (re.compile(r"\bntk\b"), "znet"),
)

# ---------------------------------------------------------------------------
# znet 里不存在的符号：脚本照旧改名，但需要人工补垫片
# ---------------------------------------------------------------------------

GAP_SYMBOLS = {
    "ntk_poller_current": (
        "znet 未提供 zn_poller_current()（TLS 记录当前线程 bind 的 poller）；"
        "建议在 nb 侧用 TLS 垫片，或推动 znet 在 zn_poller_bind_thread 里补"
    ),
    "ntk_socket_sendto_batch": (
        "znet 未提供 UDP 批量发送（原实现走 sendmmsg）；可用 zn_socket_sendto 循环兜底"
    ),
    "ntk_socket_recvfrom_batch": (
        "znet 未提供 UDP 批量接收（原实现走 recvmmsg）；可用 zn_socket_recvfrom 循环兜底"
    ),
    "ntk_udp_dgram_t": "znet socket.h 无此类型；随批量发送接口一起缺失",
    "ntk_udp_recv_t": "znet socket.h 无此类型；随批量接收接口一起缺失",
}

INCLUDE_RE = re.compile(
    r"^(?P<head>[ \t]*#[ \t]*include[ \t]*)(?P<open>[<\"])(?P<path>[^>\"\r\n]+)(?P<close>[>\"])",
    re.MULTILINE,
)

NTK_REF_RE = re.compile(r"ntk|NTK")


def warn(msg):
    print("warning: %s" % msg, file=sys.stderr)


# ---------------------------------------------------------------------------
# 文件遍历与过滤
# ---------------------------------------------------------------------------


def norm(path):
    return os.path.normpath(path).replace("\\", "/")


def is_skipped_dir(name):
    if name in SKIP_DIR_NAMES:
        return True
    for prefix in SKIP_DIR_PREFIXES:
        if name == prefix or name.startswith(prefix + "-"):
            return True
    return False


def is_text_file(name):
    if name in ("CMakeLists.txt", "Makefile"):
        return True
    return os.path.splitext(name)[1].lower() in TEXT_SUFFIXES


def iter_files(root, only, skip):
    """遍历 root 下的文本文件，返回相对的 posix 路径列表。"""
    only = [norm(o) for o in only]
    skip = [norm(s) for s in skip] + [SELF_REL_PATH]
    self_abs = os.path.abspath(__file__)
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = norm(os.path.relpath(dirpath, root))
        if rel_dir == ".":
            rel_dir = ""
        dirnames[:] = sorted(
            d for d in dirnames
            if not is_skipped_dir(d)
            and not _under_any(norm(os.path.join(rel_dir, d)), skip)
        )
        for name in sorted(filenames):
            if not is_text_file(name):
                continue
            rel = norm(os.path.join(rel_dir, name)) if rel_dir else name
            if only and not _under_any(rel, only):
                continue
            if _under_any(rel, skip):
                continue
            # 双保险：无论 root 是什么，都不改写脚本自身
            if os.path.abspath(os.path.join(root, rel)) == self_abs:
                continue
            found.append(rel)
    return found


def _under_any(rel, paths):
    for p in paths:
        if rel == p or rel.startswith(p + "/"):
            return True
    return False


# ---------------------------------------------------------------------------
# 改写逻辑
# ---------------------------------------------------------------------------


class Stats(object):
    def __init__(self):
        self.files_scanned = 0
        self.files_changed = 0
        self.symbol_hits = 0
        self.include_hits = 0
        self.unmapped_includes = []   # (file, lineno, include)
        self.gaps = {}                # symbol -> [file, ...]
        self.per_file = []            # (file, count)


def map_include(path, znet_inc):
    """返回映射后的头文件路径；无映射时返回 None。"""
    p = path.strip().replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]

    mapped = INCLUDE_MAP.get(p)
    if mapped is not None:
        return mapped

    if "/" in p:
        top = p.split("/", 1)[0]
        if top in INCLUDE_DIR_MAP:
            cand = INCLUDE_DIR_MAP[top] + p[len(top):]
            # znet 里没有这个头文件时不要盲改，交给缺口报告
            if znet_inc and not os.path.isfile(os.path.join(znet_inc, cand)):
                return None
            return cand
    return None


def rewrite_includes(text, rel, stats, znet_inc):
    def repl(m):
        path = m.group("path")
        mapped = map_include(path, znet_inc)
        if mapped is None:
            if is_ntk_header_ref(path):
                lineno = text.count("\n", 0, m.start()) + 1
                stats.unmapped_includes.append((rel, lineno, path))
            return m.group(0)
        stats.include_hits += 1
        return "%s<%s>" % (m.group("head"), mapped)

    return INCLUDE_RE.sub(repl, text)


def is_ntk_header_ref(path):
    p = path.strip().replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    if p in INCLUDE_MAP or p.startswith("ntk"):
        return True
    top = p.split("/", 1)[0]
    return "/" in p and top in ("net", "poller", "thread", "util")


def rewrite_text(text, rel, stats, znet_inc, keep_word_ntk):
    # ① include 指令先处理：<ntk.h> 不能被后面的裸词规则改成 <znet.h>
    new = rewrite_includes(text, rel, stats, znet_inc)

    # ② 标识符
    for rx, replacement in SYMBOL_RES:
        res, n = rx.subn(replacement, new)
        if n:
            stats.symbol_hits += n
            new = res

    # ③ 路径与裸词
    for rx, replacement in WORD_RULES:
        if keep_word_ntk and rx.pattern == r"\bntk\b":
            continue
        new = rx.sub(replacement, new)

    return new


def collect_gaps(text, rel, stats):
    for symbol in GAP_SYMBOLS:
        mapped = map_gap_symbol(symbol)
        if mapped in text:
            stats.gaps.setdefault(symbol, []).append(rel)


def map_gap_symbol(symbol):
    for rx, replacement in SYMBOL_RES:
        symbol = rx.sub(replacement, symbol)
    return symbol


def process_file(root, rel, stats, args, znet_inc):
    abs_path = os.path.join(root, rel)
    try:
        with open(abs_path, "r", encoding="utf-8", newline="") as f:
            text = f.read()
    except (UnicodeDecodeError, OSError) as exc:
        warn("跳过 %s：%s" % (rel, exc))
        return
    stats.files_scanned += 1

    new = rewrite_text(text, rel, stats, znet_inc, args.keep_word_ntk)
    collect_gaps(new, rel, stats)

    if new == text:
        return
    stats.files_changed += 1
    stats.per_file.append((rel, _changed_lines(text, new)))
    if args.dry_run:
        return
    with open(abs_path, "w", encoding="utf-8", newline="") as f:
        f.write(new)


def _changed_lines(old, new):
    """统计改动行数（行数变化时按较大者计）。"""
    a = old.splitlines()
    b = new.splitlines()
    if len(a) != len(b):
        return max(len(a), len(b))
    return sum(1 for x, y in zip(a, b) if x != y)


# ---------------------------------------------------------------------------
# 残留检查与 3rdparty/ntk 清理
# ---------------------------------------------------------------------------


def find_residuals(root, only, skip):
    """找出仍然提到 ntk 的文本文件行（3rdparty 之外）。"""
    hits = []
    for rel in iter_files(root, only, skip):
        abs_path = os.path.join(root, rel)
        try:
            with open(abs_path, "r", encoding="utf-8") as f:
                for lineno, line in enumerate(f, 1):
                    if NTK_REF_RE.search(line):
                        hits.append((rel, lineno, line.rstrip("\r\n")))
        except (UnicodeDecodeError, OSError):
            continue
    return hits


def delete_ntk_tree(root, args):
    ntk_dir = os.path.join(root, "3rdparty", "ntk")
    if not os.path.isdir(ntk_dir):
        return
    if args.keep_ntk:
        print("保留 3rdparty/ntk（--keep-ntk）")
        return

    # 删除是全树动作：不看 --only/--skip，只有整棵树都没有 ntk 引用时才删，
    # 否则局部迁移（或还没迁的目录）会把依赖删掉。
    residuals = find_residuals(root, [], [])
    if residuals and not args.force:
        print("跳过删除 3rdparty/ntk：树里仍有 %d 处 ntk 引用（用 --force 强制删除）"
              % len(residuals))
        for rel, lineno, line in residuals[:10]:
            print("  %s:%d: %s" % (rel, lineno, line.strip()[:100]))
        if len(residuals) > 10:
            print("  ...（共 %d 处）" % len(residuals))
        return

    if args.dry_run:
        print("dry-run：将删除 3rdparty/ntk")
        return
    shutil.rmtree(ntk_dir)
    print("已删除 3rdparty/ntk")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="把 ntk 调用机械改名为 znet（幂等）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="映射表与缺口清单见脚本头注释。",
    )
    parser.add_argument("--root", default=REPO_ROOT, help="仓库根目录（默认：脚本上一级）")
    parser.add_argument("--only", action="append", default=[], metavar="PATH",
                        help="只处理这些相对路径（可重复）")
    parser.add_argument("--skip", action="append", default=[], metavar="PATH",
                        help="跳过这些相对路径（可重复）")
    parser.add_argument("--keep-ntk", action="store_true", help="不删除 3rdparty/ntk")
    parser.add_argument("--keep-word-ntk", action="store_true",
                        help="不替换裸露的 ntk 单词（只改标识符与 include）")
    parser.add_argument("--force", action="store_true", help="有残留时也删除 3rdparty/ntk")
    parser.add_argument("--dry-run", action="store_true", help="不写盘、不删目录")
    parser.add_argument("--check", action="store_true", help="只检查残留，不改文件")
    parser.add_argument("--verbose", "-v", action="store_true", help="打印每文件改动行数")
    args = parser.parse_args(argv)

    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        print("error: 根目录不存在：%s" % root, file=sys.stderr)
        return 2

    znet_inc = os.path.join(root, "3rdparty", "znet", "include")
    if not os.path.isdir(znet_inc):
        znet_inc = None
        warn("找不到 3rdparty/znet/include，include 映射按内置表盲改")

    stats = Stats()

    if not args.check:
        for rel in iter_files(root, args.only, args.skip):
            process_file(root, rel, stats, args, znet_inc)
        print("%s：扫描 %d 个文件，改动 %d 个，标识符替换 %d 处，include 改写 %d 处"
              % ("dry-run" if args.dry_run else "迁移",
                 stats.files_scanned, stats.files_changed, stats.symbol_hits,
                 stats.include_hits))
        if args.verbose:
            for rel, lines in stats.per_file:
                print("  %-60s %d 行" % (rel, lines))
        delete_ntk_tree(root, args)

    # 残留检查按本次迁移范围（--only/--skip）报告
    residuals = find_residuals(root, list(args.only), list(args.skip))
    rc = 0

    if stats.unmapped_includes:
        print("\n未映射的 include（znet 无对应头文件）：")
        for rel, lineno, path in stats.unmapped_includes:
            print("  %s:%d: <%s>" % (rel, lineno, path))
        rc = 1

    if stats.gaps:
        print("\n缺口清单（znet 无对应符号，已 1:1 改名，需人工垫片）：")
        for symbol in sorted(stats.gaps):
            note = GAP_SYMBOLS[symbol]
            print("  %s -> %s" % (symbol, map_gap_symbol(symbol)))
            print("      用在：%s" % "、".join(sorted(set(stats.gaps[symbol]))))
            print("      建议：%s" % note)
        rc = 1

    if residuals:
        print("\n仍有 ntk 引用（%d 处）：" % len(residuals))
        for rel, lineno, line in residuals[:40]:
            print("  %s:%d: %s" % (rel, lineno, line.strip()[:100]))
        if len(residuals) > 40:
            print("  ...（共 %d 处，可用 grep 复核）" % len(residuals))
        rc = 1
    else:
        print("\n自检通过：本次范围内已无 ntk 引用")

    if args.check and rc == 0:
        print("check：无需迁移")
    return rc


if __name__ == "__main__":
    sys.exit(main())
