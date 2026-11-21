#!/usr/bin/env python3
"""
gen_apidoc.py — 从 C 源码注释生成 API 参考文档（类 rustdoc）

用法:
  python3 tools/gen_apidoc.py                    # 生成 docs/api/ 下的 md/json/html
  python3 tools/gen_apidoc.py --check            # 只统计，不写文件
  python3 tools/gen_apidoc.py --format md        # 只生成 Markdown
  python3 tools/gen_apidoc.py --format html      # 只生成 HTML
  python3 tools/gen_apidoc.py --min-coverage 50  # 覆盖率低于 50% 时 --check 失败
  python3 tools/gen_apidoc.py --include-tests    # 同时扫描 tests/unit-test

注释框架:
  生成器使用 Doxygen 风格的文档注释约定，但**同时兼容项目里已有的中文
  `/* ... */` 块注释**（Doxygen 本身不会把普通块注释当作文档，这个脚本会）。

  Doxygen 风格（推荐）:

      /**
       * @brief 从等待队列里摘掉一个任务。
       * @param queue 目标等待队列。
       * @return 被摘下的任务；队列为空时返回 NULL。
       * @note 调用者必须持有 queue->lock。
       */
      task_t *waitq_pop(wait_queue_t *queue);

  普通中文注释（历史代码，也会被收录）:

      /*
         条件检查、入等待队列与置 BLOCKED 全在 mutex->lock 下完成。
         这是修掉旧实现丢唤醒的关键。
      */
      void mutex_lock(mutex_t *mutex)

  只认**紧挨着函数定义（中间可以隔空行）的上方注释**。形如
  `/* ── mutex_lock ── */` 的装饰性分节标题会被跳过；如果分节标题上方还有
  正文注释，会继续向上找到它。

为什么不用 Doxygen:
  - 仓库里 60% 以上的函数只有普通 `/* */` 注释，Doxygen 直接忽略；
  - 本脚本零依赖（只用 Python 标准库），`make apidoc` 在任何机器上都能跑；
  - 输出是单页可搜索 HTML，交互接近 rustdoc，且能嵌入 CI/Artifact。

扫描范围:
  kernel driver fs lib boot platforms apps include tools
  默认排除 third_party / build / imgs / docs / tests/ltp / apps/rknn-aarch64
"""

from __future__ import annotations

import argparse
import datetime
import html
import json
import os
import re
import sys
from collections import OrderedDict

# ─── 扫描配置 ────────────────────────────────────────────────────────────────

DEFAULT_ROOTS = [
    "kernel",
    "driver",
    "fs",
    "lib",
    "boot",
    "platforms",
    "apps",
    "include",
    "tools",
]

EXCLUDE_DIRS = {
    ".git",
    ".artifacts",
    "build",
    "third_party",
    "imgs",
    "docs",
    "node_modules",
    "__pycache__",
}

EXCLUDE_PREFIXES = (
    "tests/ltp/",
    "tests/.artifacts/",
    "apps/rknn-aarch64/",  # 厂商 SDK 头文件，不属于本项目的 API
    # 已收编为 first-party 但保持上游风格的文件系统核心：内部 API 不生成
    # 项目 API 文档；端口层（fs/ext4/port、fs/fat/port）仍会被扫描。
    "fs/ext4/lwext4/",
    "fs/fat/fatfs/",
)

SOURCE_EXTS = (".c", ".h")

# 顶层语句里出现这些标识符开头的括号组时，不是函数参数表
ATTR_KEYWORDS = {
    "__attribute__",
    "__attribute",
    "__declspec",
    "__asm__",
    "__asm",
    "asm",
    "__typeof__",
    "__typeof",
    "typeof",
    "__extension__",
    "__aligned__",
    "__section__",
    "__packed__",
    "__unused__",
}

CONTROL_KEYWORDS = {
    "if",
    "for",
    "while",
    "switch",
    "return",
    "sizeof",
    "va_arg",
    "va_start",
    "va_end",
    "defined",
    "do",
    "else",
    "case",
    "goto",
    "catch",
}

DEFAULT_EXCLUDE_STORAGE = {"static", "inline", "extern", "__inline__", "__inline"}

TAG_RE = re.compile(
    r"^[@\\](brief|details|param|arg|return|returns|retval|note|warning|see|"
    r"deprecated|since|todo)\b\s*(.*)$"
)


# ─── 源码视图：抹掉注释/字符串，保留行号与列 ─────────────────────────────────


def blank_comments_and_strings(text: str) -> str:
    """返回等行数的源码视图：注释内容被抹掉，字符串/字符字面量内容被空格填掉。

    只用于语法扫描（找函数、配括号）。注释正文与字符串内容都不参与解析，
    这样 `"}"` 不会把花括号计数搞乱。
    """
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                j = n - 2
            seg = text[i : j + 2]
            out.append("\n" * seg.count("\n"))
            i = j + 2
        elif text.startswith("//", i):
            j = text.find("\n", i)
            if j < 0:
                j = n
            out.append("")
            i = j
        elif text[i] == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    break
                j += 1
            seg = text[i : j + 1]
            body = " " * max(0, len(seg) - 2)
            out.append('"' + body + '"' if len(seg) >= 2 else '""')
            i = j + 1
        elif text[i] == "'":
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "'":
                    break
                j += 1
            seg = text[i : j + 1]
            body = " " * max(0, len(seg) - 2)
            out.append("'" + body + "'" if len(seg) >= 2 else "''")
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


# ─── 注释清洗 ────────────────────────────────────────────────────────────────


def clean_comment(text: str) -> str:
    """去掉注释定界符、每行前导 `*`，把纯装饰线替换成空行。"""
    lines: list[str] = []
    for raw in text.splitlines():
        s = raw.strip()
        if s.startswith("/**") or s.startswith("/*!") or s.startswith("/*"):
            s = re.sub(r"^/\*+[!<]?", "", s)
        if s.endswith("*/"):
            s = s[:-2]
        if s.startswith("*"):
            s = s[1:]
            if s.startswith(" "):
                s = s[1:]
        elif s.startswith("//"):
            s = re.sub(r"^//+[!<]?", "", s)
            if s.startswith(" "):
                s = s[1:]
        s = s.rstrip()
        # 纯装饰线（──── / ==== / ---- / ****）不承载信息
        if re.fullmatch(r"[\s─━＝=\-#*_~]+", s):
            s = ""
        lines.append(s)

    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()

    # 连续空行压成一个
    out: list[str] = []
    for line in lines:
        if not line.strip() and out and not out[-1].strip():
            continue
        out.append(line)
    return "\n".join(out)


def is_comment_meaningful(text: str, func_name: str) -> bool:
    """判断清理后的注释是否是一段真正的说明，而不是许可证/分节标题/文件名。"""
    t = text.strip()
    if not t:
        return False
    first = t.splitlines()[0].strip()
    if "SPDX-License-Identifier" in t or "Copyright" in t:
        return False
    # 模块头：`kernel/task/mutex.c — ...`
    if re.match(r"^\S+\.(?:c|h)\b", first):
        return False
    # 单行装饰性分节标题：`── mutex_lock ──────`
    if len(t.splitlines()) == 1 and re.search(r"[─━=\-]{2,}", text):
        return False
    plain = re.sub(r"[^0-9A-Za-z_一-鿿]+", "", t)
    if not plain:
        return False
    if plain == func_name:
        return False
    return True


def parse_doc_comment(text: str, func_name: str) -> dict | None:
    """把清理后的文档注释解析为结构化字段（兼容 Doxygen 标签）。"""
    text = clean_comment(text)
    if not is_comment_meaningful(text, func_name):
        return None

    items: list[list[str]] = []  # [kind, payload]
    current: int | None = None
    for raw in text.splitlines():
        s = raw.strip()
        if not s:
            # 空行是段落分隔；同一段内的换行保留为普通换行
            if current is not None and items[current][0] in ("text", "details", "brief"):
                items[current][1] = items[current][1].rstrip() + "\n\n"
            current = None
            continue
        m = TAG_RE.match(s)
        if m:
            items.append([m.group(1).lower(), m.group(2).strip()])
            current = len(items) - 1
            continue
        if current is not None:
            payload = items[current][1]
            sep = "" if not payload or payload.endswith("\n") else "\n"
            items[current][1] = payload + sep + s
        else:
            items.append(["text", s])
            current = len(items) - 1

    desc: list[str] = []
    brief = ""
    params: dict[str, str] = {}
    returns = ""
    notes: list[dict] = []

    for kind, payload in items:
        payload = payload.strip()
        if not payload:
            continue
        if kind == "brief":
            brief = payload
        elif kind in ("details", "text"):
            desc.append(payload)
        elif kind in ("param", "arg"):
            m = re.match(r"^(?:\[[^\]]*\]\s*)?([A-Za-z_]\w*)\s*(.*)$", payload, re.S)
            if m:
                params[m.group(1)] = m.group(2).strip()
        elif kind in ("return", "returns", "retval"):
            returns = (returns + "\n" + payload).strip() if returns else payload
        elif kind in ("note", "warning", "see", "deprecated", "since", "todo"):
            notes.append({"kind": kind, "text": payload})
        else:
            desc.append(payload)

    description = "\n\n".join(d for d in desc if d.strip()).strip()
    if not description and not brief and not params and not returns and not notes:
        return None
    if not brief:
        # 第一段作为摘要；含句号/中文句号时截到第一句
        first = description.split("\n")[0].strip() if description else ""
        if not first and notes:
            first = notes[0]["text"].split("\n")[0].strip()
        brief = first

    return {
        "brief": brief,
        "description": description,
        "params": params,
        "returns": returns,
        "notes": notes,
    }


# ─── 定位函数上方的注释块 ────────────────────────────────────────────────────


def _comment_block_above(raw_lines: list[str], start_line: int):
    """返回 (block_text, block_start_line) 或 None。

    start_line 是函数定义首行的 1-based 行号。允许中间夹空行。
    """
    i = start_line - 2
    while i >= 0 and not raw_lines[i].strip():
        i -= 1
    if i < 0:
        return None

    s = raw_lines[i].strip()
    if s.startswith("//"):
        end = i
        while i > 0 and raw_lines[i - 1].strip().startswith("//"):
            i -= 1
        return "\n".join(raw_lines[i : end + 1]), i
    if s.endswith("*/"):
        end = i
        while i >= 0 and "/*" not in raw_lines[i]:
            i -= 1
        if i < 0:
            return None
        # 注释必须从行首开始；`#include "x.h" /* 说明 */` 不是函数文档
        before = raw_lines[i][: raw_lines[i].find("/*")].strip()
        if before:
            return None
        return "\n".join(raw_lines[i : end + 1]), i
    return None


def extract_doc_above(raw_lines: list[str], start_line: int, func_name: str):
    """取函数上方最近的正文注释；遇到装饰性分节标题时继续向上找。"""
    line = start_line
    for _ in range(3):
        block = _comment_block_above(raw_lines, line)
        if block is None:
            return None
        text, block_start = block
        parsed = parse_doc_comment(text, func_name)
        if parsed is not None:
            return parsed
        cleaned = clean_comment(text)
        # 只对“纯分节标题”继续向上；其它情况（许可证/模块头）直接停
        plain = re.sub(r"[^0-9A-Za-z_一-鿿]+", "", cleaned)
        if not plain or plain == func_name or (
            len(cleaned.splitlines()) == 1 and re.search(r"[─━=\-]{2,}", text)
        ):
            line = block_start  # 从这个块上方继续
            continue
        return None
    return None


def extract_module_doc(raw_lines: list[str], first_api_line: int):
    """取文件头部的模块说明；不越过第一个函数/类型定义。"""
    # 只看第一个函数/类型之前的部分；没有函数时看全文件
    limit = min(len(raw_lines), max(0, first_api_line - 1))
    first_include = next(
        (
            n
            for n, ln in enumerate(raw_lines[:limit])
            if ln.lstrip().startswith("#include")
        ),
        limit,
    )

    i = 0
    blocks: list[tuple[int, int, str]] = []
    while i < limit:
        line = raw_lines[i]
        if "/*" in line:
            # 只收行首的块注释；`#include "x.h" /* 说明 */` 这种行内注释不算模块说明
            if line[: line.find("/*")].strip():
                i += 1
                continue
            start = i
            if "*/" in line[line.find("/*") + 2 :]:
                blocks.append((start, i, "\n".join(raw_lines[start : i + 1])))
                i += 1
                continue
            j = i + 1
            while j < limit and "*/" not in raw_lines[j]:
                j += 1
            blocks.append((start, min(j, limit - 1), "\n".join(raw_lines[start : j + 1])))
            i = j + 1
            continue
        if line.lstrip().startswith("//") and not line.lstrip().startswith("///"):
            start = i
            j = i
            while j + 1 < limit and raw_lines[j + 1].strip().startswith("//"):
                j += 1
            blocks.append((start, j, "\n".join(raw_lines[start : j + 1])))
            i = j + 1
            continue
        i += 1

    # 第一个 #include 之后的第一条非 #include 预处理指令：它通常意味着
    # “模块说明已经结束，进入条件编译/配置区”，之后的注释不再当模块说明。
    first_post_include_pp = next(
        (
            n
            for n in range(first_include, limit)
            if raw_lines[n].lstrip().startswith("#")
            and not raw_lines[n].lstrip().startswith("#include")
        ),
        limit,
    )

    for start, _end, text in blocks:
        cleaned = clean_comment(text)
        # SPDX/Copyright 可能和模块说明在同一个块里（如 apps/c/rtctl.c），
        # 只去掉许可证行，不要把整块丢掉。
        cleaned = "\n".join(
            ln
            for ln in cleaned.splitlines()
            if "SPDX-License-Identifier" not in ln and "Copyright" not in ln
        ).strip()
        if not cleaned.strip():
            continue
        first = cleaned.splitlines()[0].strip()
        rest = "\n".join(cleaned.splitlines()[1:]).strip()
        m = re.match(r"^(?:\S+\.(?:c|h))\s*[—–-]\s*(.+)$", first)
        if m:
            return {"title": m.group(1).strip(), "description": rest}
        # 第一个 #include 之前、或 include 之后但尚未进入条件编译区的注释，
        # 可以当模块说明；之后只认带文件名前缀的标题。
        if (start < first_include or start < first_post_include_pp) and len(first) <= 100:
            return {"title": first, "description": rest}
    return {"title": "", "description": ""}


# ─── 函数签名解析 ────────────────────────────────────────────────────────────


def _find_param_list(stmt: str):
    """找到函数参数表的左括号位置和函数名；处理前置 __attribute__。"""
    s = stmt
    while True:
        m = re.match(
            r"^\s*(?:__attribute__|__declspec)\s*\(\((?:[^()]|\([^()]*\))*\)\)\s*",
            s,
        )
        if not m:
            break
        s = s[m.end() :]

    depth = 0
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == "(":
            j = i - 1
            while j >= 0 and s[j].isspace():
                j -= 1
            k = j
            while k >= 0 and (s[k].isalnum() or s[k] == "_"):
                k -= 1
            ident = s[k + 1 : j + 1]
            if depth == 0 and ident and ident not in ATTR_KEYWORDS:
                return s, k + 1, j + 1, i
            depth += 1
        elif c == ")":
            if depth > 0:
                depth -= 1
        i += 1
    return s, None, None, None


def split_params(params: str) -> list[dict]:
    """拆分参数列表；只用于文档展示，不追求完整的 C 类型解析。"""
    params = params.strip()
    if not params or params == "void":
        return []

    parts: list[str] = []
    depth = 0
    cur: list[str] = []
    for ch in params:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth = max(0, depth - 1)
        if ch == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur).strip())

    out: list[dict] = []
    for p in parts:
        if not p:
            continue
        if p == "...":
            out.append({"name": "...", "type": "..."})
            continue
        # 函数指针参数: void (*fn)(int)
        m = re.search(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)", p)
        if m:
            name = m.group(1)
            type_text = (p[: m.start()] + p[m.end() :]).strip()
            out.append({"name": name, "type": type_text or p})
            continue
        # 数组参数: char buf[32]
        m2 = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*$", p)
        if m2:
            name = m2.group(1)
            type_text = p[: m2.start(1)].strip()
            # `struct foo` 这种没有参数名的场合，最后一个词是类型名
            if type_text in ("struct", "union", "enum", "const", "volatile", "unsigned", "signed"):
                out.append({"name": "", "type": p})
            else:
                out.append({"name": name, "type": type_text or p})
        else:
            out.append({"name": "", "type": p})
    return out


def parse_function_stmt(stmt: str):
    """从一条已到 `{` 的顶层语句解析函数定义；不是函数返回 None。"""
    s = stmt.strip()
    if s.endswith("{"):
        s = s[:-1].rstrip()
    s, name_start, name_end, paren = _find_param_list(s)
    if name_start is None:
        return None
    name = s[name_start:name_end]
    if name in CONTROL_KEYWORDS or name in ATTR_KEYWORDS:
        return None

    # 匹配参数表的右括号
    depth = 0
    end = None
    for j in range(paren, len(s)):
        if s[j] == "(":
            depth += 1
        elif s[j] == ")":
            depth -= 1
            if depth == 0:
                end = j
                break
    if end is None:
        return None

    before = s[:name_start].strip()
    if not before or "=" in before:
        return None
    if before.startswith("typedef"):
        return None
    if any(tok in CONTROL_KEYWORDS for tok in before.split()):
        return None

    after = s[end + 1 :].strip()
    # 允许尾随属性/asm/宏；出现 `:` 等说明不是普通函数定义
    if after and not re.match(
        r"^(?:__attribute__|__asm__|__asm|asm|__declspec|__extension__)\b", after
    ):
        return None

    params_text = s[paren + 1 : end].strip()
    # 展示签名保留原始的前置属性（如 __attribute__((weak, noinline))）；
    # 解析用的 s 已剥掉属性，不能直接拿 s 生成展示签名。
    raw_sig = stmt.strip()
    if raw_sig.endswith("{"):
        raw_sig = raw_sig[:-1].rstrip()
    sig = re.sub(r"\s+", " ", raw_sig).strip()
    # 去掉函数名前面的多余空白（保留 * 与名字的常见写法）
    linkage = []
    for tok in ("static", "inline", "extern"):
        if re.search(rf"\b{tok}\b", before):
            linkage.append(tok)

    return {
        "name": name,
        "signature": sig,
        "return_type": before,
        "params": split_params(params_text),
        "linkage": linkage,
    }


def _find_body_range(raw_text: str, start_line: int):
    """返回函数体的 [body_start, body_end) 原始字符偏移；找不到返回 (None, None)。

    直接扫描原始文本，跳过注释/字符串，因此不受“同一行前面有注释导致列偏移”
    的影响。body_start 指向 `{` 之后，body_end 指向匹配的 `}` 之前。
    """
    line_starts = [0]
    for idx, ch in enumerate(raw_text):
        if ch == "\n":
            line_starts.append(idx + 1)
    if start_line - 1 >= len(line_starts):
        return None, None

    i = line_starts[start_line - 1]
    n = len(raw_text)
    depth = 0
    body_start = body_end = None
    while i < n:
        if raw_text.startswith("/*", i):
            j = raw_text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if raw_text.startswith("//", i):
            j = raw_text.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        if raw_text[i] == '"':
            i += 1
            while i < n:
                if raw_text[i] == "\\":
                    i += 2
                    continue
                if raw_text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if raw_text[i] == "'":
            i += 1
            while i < n:
                if raw_text[i] == "\\":
                    i += 2
                    continue
                if raw_text[i] == "'":
                    i += 1
                    break
                i += 1
            continue
        if raw_text[i] == "{":
            if depth == 0:
                body_start = i + 1
            depth += 1
        elif raw_text[i] == "}":
            depth -= 1
            if depth == 0 and body_start is not None:
                body_end = i
                break
        i += 1
    return body_start, body_end


def extract_body_notes(raw_text: str, start_line: int, func_name: str) -> list[dict]:
    """收集函数体内部的每条注释，作为可展开的“函数内备注”。

    每条独立注释块是一条备注；行尾注释会标记 inline。只跳过纯装饰线，
    其余原样保留（行号是源码里的绝对行号）。
    """
    body_start, body_end = _find_body_range(raw_text, start_line)
    if body_start is None or body_end is None or body_end <= body_start:
        return []

    body = raw_text[body_start:body_end]
    line_no = raw_text.count("\n", 0, body_start) + 1
    line_start = 0  # 当前行在 body 内的起始偏移
    notes: list[dict] = []
    i = 0
    n = len(body)
    while i < n:
        if body.startswith("/*", i):
            j = body.find("*/", i + 2)
            j = n - 2 if j < 0 else j
            seg = body[i : j + 2]
            inline = bool(body[line_start:i].strip())
            cleaned = clean_comment(seg).strip()
            if cleaned and cleaned != func_name:
                notes.append({"line": line_no, "text": cleaned, "inline": inline})
            cnt = seg.count("\n")
            if cnt:
                line_start = i + seg.rfind("\n") + 1
            line_no += cnt
            i = j + 2
            continue
        if body.startswith("//", i):
            j = body.find("\n", i)
            j = n if j < 0 else j
            seg = body[i:j]
            inline = bool(body[line_start:i].strip())
            cleaned = clean_comment(seg).strip()
            if cleaned and cleaned != func_name:
                notes.append({"line": line_no, "text": cleaned, "inline": inline})
            i = j
            continue
        if body[i] == '"':
            i += 1
            while i < n:
                if body[i] == "\\":
                    i += 2
                    continue
                if body[i] == '"':
                    i += 1
                    break
                if body[i] == "\n":
                    line_no += 1
                    line_start = i + 1
                i += 1
            continue
        if body[i] == "'":
            i += 1
            while i < n:
                if body[i] == "\\":
                    i += 2
                    continue
                if body[i] == "'":
                    i += 1
                    break
                i += 1
            continue
        if body[i] == "\n":
            line_no += 1
            line_start = i + 1
        i += 1
    return notes


def _first_terminator(stmt: str):
    """返回 (终止符, 位置)：顶层（括号外）第一个 `;` 或 `{`。"""
    paren = 0
    for i, ch in enumerate(stmt):
        if ch in "([":
            paren += 1
        elif ch in ")]":
            paren = max(0, paren - 1)
        elif paren == 0 and ch in ";{":
            return ch, i
    return None, -1


def find_functions(relpath: str, raw_text: str):
    """扫描一个文件的顶层函数定义，返回 (functions, warnings)。"""
    view = blank_comments_and_strings(raw_text)
    lines = view.splitlines()
    raw_lines = raw_text.splitlines()
    funcs: list[dict] = []
    warnings: list[str] = []

    i = 0
    depth = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        # 预处理指令（含 `\` 续行的宏）整体跳过，避免把宏体当函数/改变花括号深度
        if stripped.startswith("#"):
            while i < len(lines) and lines[i].rstrip().endswith("\\"):
                i += 1
            i += 1
            continue
        if (
            depth == 0
            and stripped
            and "(" in line
            and not stripped.endswith(";")
            and not re.match(r"^[A-Z][A-Z0-9_]*\s*\(", stripped)  # FS_WRAP(...) 等
            # 宏调用的续行（以 ( ) , 开头），不是新的顶层函数定义
            and not stripped.startswith(("(", ")", ","))
        ):
            # 多行返回类型：把上一行类型/限定符拼回来（最多一行）
            start = i
            prefix: list[str] = []
            k = i - 1
            while k >= 0:
                p = lines[k].strip()
                if not p:
                    break
                if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_\s\*]*", p) and "(" not in p:
                    prefix.insert(0, p)
                    start = k
                    k -= 1
                    if len(prefix) >= 1:
                        break
                    continue
                break

            parts: list[str] = prefix + [line]
            j = i
            term = None
            paren_depth = 0
            while j < len(lines) and j - i <= 40:
                if j != i:
                    parts.append(lines[j])
                for ch in lines[j]:
                    if ch in "([":
                        paren_depth += 1
                    elif ch in ")]":
                        paren_depth = max(0, paren_depth - 1)
                    elif paren_depth == 0 and ch in ";{}":
                        term = ch
                        break
                if term:
                    break
                j += 1

            full = " ".join(parts)
            tchar, tpos = _first_terminator(full)
            if tchar == "{":
                stmt = full[:tpos].strip()
                parsed = parse_function_stmt(stmt)
                if parsed:
                    doc = extract_doc_above(raw_lines, start + 1, parsed["name"])
                    parsed["line"] = start + 1
                    parsed["doc"] = doc
                    parsed["documented"] = doc is not None
                    parsed["body_notes"] = extract_body_notes(
                        raw_text, start + 1, parsed["name"]
                    )
                    funcs.append(parsed)
                elif (
                    "=" in stmt
                    or stmt.startswith(("struct ", "union ", "enum ", "typedef "))
                ):
                    # 全局初始化器（DRIVER_ENTRY(...) = {...}）等，不是函数
                    pass
                else:
                    warnings.append(
                        f"{relpath}:{start + 1}: 无法解析的顶层语句（已跳过）"
                    )
                depth += full.count("{") - full.count("}")
            elif tchar == ";":
                depth += full.count("{") - full.count("}")
            else:
                # 没找到终止符（超过 40 行或遇到 EOF），保守跳过
                warnings.append(
                    f"{relpath}:{start + 1}: 顶层语句没有找到 `;` 或 `{{`（已跳过）"
                )
                return funcs, warnings
            if depth < 0:
                depth = 0
            i = j + 1
            continue

        depth += line.count("{") - line.count("}")
        if depth < 0:
            depth = 0
        i += 1

    return funcs, warnings


# ─── 文件/模块收集 ───────────────────────────────────────────────────────────


def iter_sources(root: str, include_tests: bool = False):
    """按确定的顺序遍历一方 C 源文件。"""
    roots = list(DEFAULT_ROOTS)
    if include_tests:
        roots.append("tests/unit-test")

    for top in roots:
        top_path = os.path.join(root, top)
        if not os.path.isdir(top_path):
            continue
        for cur, dirs, files in os.walk(top_path):
            dirs[:] = sorted(d for d in dirs if d not in EXCLUDE_DIRS)
            for name in sorted(files):
                if not name.endswith(SOURCE_EXTS):
                    continue
                full = os.path.join(cur, name)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                if any(rel.startswith(p) for p in EXCLUDE_PREFIXES):
                    continue
                if rel.startswith("tests/") and not include_tests:
                    continue
                yield rel, full


def build_docs(root: str, include_tests: bool = False):
    modules: list[dict] = []
    warnings: list[str] = []

    for rel, full in iter_sources(root, include_tests):
        try:
            with open(full, "r", encoding="utf-8", errors="replace") as f:
                raw = f.read()
        except OSError as e:
            warnings.append(f"{rel}: 读取失败: {e}")
            continue

        funcs, w = find_functions(rel, raw)
        warnings.extend(w)
        first_api = min((f["line"] for f in funcs), default=len(raw.splitlines()) + 1)
        mod_doc = extract_module_doc(raw.splitlines(), first_api)

        subsystem = rel.split("/", 1)[0]
        modules.append(
            {
                "path": rel,
                "subsystem": subsystem,
                "title": mod_doc["title"],
                "description": mod_doc["description"],
                "loc": raw.count("\n") + (0 if raw.endswith("\n") else 1),
                "functions": funcs,
            }
        )

    apply_auto_summaries(modules, root)
    return modules, warnings


def apply_auto_summaries(modules: list[dict], root: str):
    """叠加可选的自动摘要文件（docs/api/auto_summaries.json）。

    格式:
      { "kernel/rt/rt_worker.c::rtw_clamp_i64": "把 64 位值限制在 [lo, hi]。" }
    或 { "...": {"brief": "...", "description": "..."} }

    自动摘要只补在没有人工注释的函数上，展示时会带 “自动摘要” 标记。
    """
    path = os.path.join(root, "docs", "api", "auto_summaries.json")
    if not os.path.isfile(path):
        return
    try:
        with open(path, "r", encoding="utf-8") as f:
            overlay = json.load(f)
    except (OSError, json.JSONDecodeError):
        return

    for mod in modules:
        for fn in mod["functions"]:
            if fn.get("doc"):
                continue
            key = f"{mod['path']}::{fn['name']}"
            val = overlay.get(key)
            if not val:
                continue
            if isinstance(val, str):
                brief = val
                desc = val
            elif isinstance(val, dict):
                brief = val.get("brief", "")
                desc = val.get("description", brief)
            else:
                continue
            fn["doc"] = {
                "brief": brief,
                "description": desc,
                "params": {},
                "returns": "",
                "notes": [],
            }
            fn["documented"] = True
            fn["auto"] = True


# ─── 统计 ────────────────────────────────────────────────────────────────────


def compute_stats(modules: list[dict]) -> dict:
    total_funcs = sum(len(m["functions"]) for m in modules)
    documented = sum(1 for m in modules for f in m["functions"] if f.get("doc"))
    auto = sum(1 for m in modules for f in m["functions"] if f.get("auto"))
    body_notes = sum(len(f.get("body_notes", [])) for m in modules for f in m["functions"])
    with_api = sum(1 for m in modules if m["functions"])
    loc = sum(m["loc"] for m in modules)

    per: "OrderedDict[str, dict]" = OrderedDict()
    for m in modules:
        s = per.setdefault(
            m["subsystem"],
            {
                "name": m["subsystem"],
                "modules": 0,
                "modules_with_api": 0,
                "functions": 0,
                "documented": 0,
                "auto": 0,
                "loc": 0,
            },
        )
        s["modules"] += 1
        s["loc"] += m["loc"]
        if m["functions"]:
            s["modules_with_api"] += 1
        for f in m["functions"]:
            s["functions"] += 1
            if f.get("doc"):
                s["documented"] += 1
            if f.get("auto"):
                s["auto"] += 1

    for s in per.values():
        s["coverage"] = round(100.0 * s["documented"] / s["functions"], 1) if s["functions"] else 0.0

    return {
        "modules_total": len(modules),
        "modules_with_api": with_api,
        "functions_total": total_funcs,
        "documented": documented,
        "auto": auto,
        "manual": documented - auto,
        "undocumented": total_funcs - documented,
        "coverage": round(100.0 * documented / total_funcs, 1) if total_funcs else 0.0,
        "body_notes": body_notes,
        "loc": loc,
        "subsystems": list(per.values()),
    }


# ─── Markdown 输出 ───────────────────────────────────────────────────────────


def _md_escape_pipe(s: str) -> str:
    return s.replace("|", "\\|")


def render_markdown(modules: list[dict], stats: dict) -> dict[str, str]:
    out: dict[str, str] = {}
    generated = datetime.date.today().isoformat()

    # 总览
    lines = [
        "# Avatar OS C API 参考",
        "",
        f"> 自动生成于 {generated} · 由 `make apidoc` 生成（`tools/gen_apidoc.py`）",
        "",
        "本文档由 C 源码中**紧挨函数定义上方的注释**生成，注释约定见 "
        "[README.md](README.md)。生成器同时兼容 Doxygen 标签和项目现有的中文 "
        "`/* ... */` 块注释。",
        "",
        "## 总览",
        "",
        "| 子系统 | 模块 | 含 API 模块 | 函数 | 人工注释 | 自动摘要 | 说明覆盖率 | 代码行数 |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for s in stats["subsystems"]:
        manual = s["documented"] - s["auto"]
        lines.append(
            f"| [{s['name']}]({s['name']}.md) | {s['modules']} | "
            f"{s['modules_with_api']} | {s['functions']} | {manual} | {s['auto']} | "
            f"{s['coverage']:.1f}% | {s['loc']} |"
        )
    lines.append(
        f"| **合计** | **{stats['modules_total']}** | **{stats['modules_with_api']}** | "
        f"**{stats['functions_total']}** | **{stats['manual']}** | **{stats['auto']}** | "
        f"**{stats['coverage']:.1f}%** | **{stats['loc']}** |"
    )
    lines += [
        "",
        f"- 函数总数：**{stats['functions_total']}**",
        f"- 人工注释：**{stats['manual']}**"
        f"（{100.0 * stats['manual'] / (stats['functions_total'] or 1):.1f}%）",
        f"- 自动摘要：**{stats['auto']}**",
        f"- 尚无说明：**{stats['undocumented']}**",
        f"- 说明覆盖率：**{stats['coverage']:.1f}%**（人工 + 自动）",
        f"- 函数内备注：**{stats['body_notes']}** 条（HTML/Markdown 里可展开）",
        "",
        "## 子系统",
        "",
    ]
    for s in stats["subsystems"]:
        lines.append(f"### [{s['name']}]({s['name']}.md)")
        lines.append("")
        n_api = sum(1 for m in modules if m["subsystem"] == s["name"] and m["functions"])
        lines.append(
            f"{s['modules']} 个源文件（{n_api} 个含函数定义）· "
            f"{s['functions']} 个函数 · 人工 {s['documented'] - s['auto']} · "
            f"自动 {s['auto']} · 说明覆盖率 {s['coverage']:.1f}%"
        )
        lines.append("")
    lines += [
        "## 如何补充注释",
        "",
        "```c",
        "/**",
        " * @brief 从等待队列里摘掉一个任务。",
        " * @param queue 目标等待队列。",
        " * @return 被摘下的任务；队列为空时返回 NULL。",
        " * @note 调用者必须持有 queue->lock。",
        " */",
        "task_t *waitq_pop(wait_queue_t *queue);",
        "```",
        "",
        "注释必须紧挨函数定义上方（中间可以有不超过 2 个分节标题）。"
        "改完运行 `make apidoc`，覆盖率会同步更新。",
        "",
    ]
    out["index.md"] = "\n".join(lines)

    # 每个子系统
    for s in stats["subsystems"]:
        sub = s["name"]
        lines = [
            f"# {sub} — C API",
            "",
            f"[← 返回总览](index.md) · {s['functions']} 个函数 · "
            f"说明覆盖率 {s['coverage']:.1f}%",
            "",
        ]
        submods = [m for m in modules if m["subsystem"] == sub and m["functions"]]
        if not submods:
            lines.append("_该子系统没有函数定义。_")
            lines.append("")
        for m in sorted(submods, key=lambda x: x["path"]):
            title = m["title"] or os.path.basename(m["path"])
            lines.append(f'<a id="mod-{_slug(m["path"])}"></a>')
            lines.append("")
            lines.append(f"## {m['path']} — {title}" if m["title"] else f"## {m['path']}")
            lines.append("")
            documented = sum(1 for f in m["functions"] if f.get("doc"))
            lines.append(
                f"`{m['path']}` · {len(m['functions'])} 个函数 · "
                f"{documented} 个有说明 · {m['loc']} 行"
            )
            lines.append("")
            if m["description"]:
                lines.append(m["description"])
                lines.append("")

            for f in m["functions"]:
                lines.append(f'<a id="fn-{_slug(m["path"])}-{f["line"]}"></a>')
                lines.append("")
                lines.append(f"### `{f['name']}`")
                lines.append("")
                lines.append("```c")
                lines.append(f["signature"])
                lines.append("```")
                lines.append("")
                doc = f.get("doc")
                if doc:
                    if f.get("auto"):
                        lines.append(
                            "> 自动摘要（来自 `docs/api/auto_summaries.json`，"
                            "不是源码中的人工注释）"
                        )
                        lines.append("")
                    if doc.get("description"):
                        lines.append(doc["description"])
                        lines.append("")
                    # 参数表只在确实有参数说明时展开，否则签名已经够了
                    if f["params"] and any(
                        doc.get("params", {}).get(p["name"]) for p in f["params"]
                    ):
                        lines.append("| 参数 | 类型 | 说明 |")
                        lines.append("| --- | --- | --- |")
                        for p in f["params"]:
                            if p["name"] == "...":
                                lines.append("| `...` | 可变参数 | — |")
                                continue
                            pdoc = doc.get("params", {}).get(p["name"], "—")
                            lines.append(
                                f"| `{p['name'] or '—'}` | "
                                f"`{_md_escape_pipe(p['type'])}` | "
                                f"{_md_escape_pipe(pdoc)} |"
                            )
                        lines.append("")
                    if doc.get("returns"):
                        lines.append(f"**返回**：{doc['returns']}")
                        lines.append("")
                    for note in doc.get("notes", []):
                        label = {
                            "note": "注意",
                            "warning": "警告",
                            "see": "参见",
                            "deprecated": "已废弃",
                            "since": "引入版本",
                            "todo": "待办",
                        }.get(note["kind"], note["kind"])
                        lines.append(f"> **{label}**：{note['text']}")
                        lines.append("")
                else:
                    lines.append(
                        "> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。"
                    )
                    lines.append("")

                body_notes = f.get("body_notes") or []
                if body_notes:
                    lines.append("<details>")
                    lines.append(
                        f"<summary>函数内备注（{len(body_notes)} 条）</summary>"
                    )
                    lines.append("")
                    for note in body_notes:
                        suffix = "（行尾）" if note.get("inline") else ""
                        lines.append(f"**第 {note['line']} 行{suffix}**")
                        lines.append("")
                        escaped = html.escape(note["text"])
                        for para in re.split(r"\n{2,}", escaped):
                            lines.append(
                                "\n".join("> " + ln for ln in para.split("\n"))
                            )
                            lines.append("")
                    lines.append("</details>")
                    lines.append("")

                lines.append(f"**位置**：`{m['path']}:{f['line']}`")
                lines.append("")

        out[f"{sub}.md"] = "\n".join(lines).rstrip() + "\n"

    return out


def _slug(text: str) -> str:
    return re.sub(r"[^0-9A-Za-z_.-]+", "-", text)


# ─── HTML 输出（rustdoc 风格单页） ───────────────────────────────────────────


def render_html(modules: list[dict], stats: dict) -> str:
    data = {
        "project": "Avatar OS",
        "generated": datetime.date.today().isoformat(),
        "stats": stats,
        "modules": [
            {
                "path": m["path"],
                "subsystem": m["subsystem"],
                "title": m["title"],
                "description": m["description"],
                "loc": m["loc"],
                "functions": [
                    {
                        "name": f["name"],
                        "signature": f["signature"],
                        "line": f["line"],
                        "linkage": f.get("linkage", []),
                        "params": f["params"],
                        "doc": f.get("doc"),
                        "auto": bool(f.get("auto")),
                        "body_notes": f.get("body_notes", []),
                    }
                    for f in m["functions"]
                ],
            }
            for m in modules
            if m["functions"]
        ],
    }
    data_json = json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace(
        "</", "<\\/"
    )
    return _HTML_TEMPLATE.replace("__API_DATA__", data_json)


_HTML_TEMPLATE = r"""<title>Avatar OS · C API 参考</title>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root {
  --bg: #f7f8fa;
  --panel: #ffffff;
  --panel-2: #f0f2f5;
  --border: #d9dde3;
  --fg: #1c2024;
  --muted: #626b76;
  --accent: #0b6e99;
  --accent-soft: #e3f1f7;
  --warn: #a35b00;
  --warn-soft: #fff3df;
  --ok: #2f7d4f;
  --code-bg: #f4f6f8;
  --shadow: 0 1px 2px rgba(20, 26, 33, .06), 0 8px 24px rgba(20, 26, 33, .06);
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg: #121619;
    --panel: #1a2025;
    --panel-2: #20272d;
    --border: #2e3840;
    --fg: #e6edf3;
    --muted: #9aa7b2;
    --accent: #62c7f2;
    --accent-soft: #16323f;
    --warn: #e2a24c;
    --warn-soft: #3a2a13;
    --ok: #7bd39f;
    --code-bg: #141a1e;
    --shadow: 0 1px 2px rgba(0, 0, 0, .35), 0 8px 24px rgba(0, 0, 0, .3);
  }
}
:root[data-theme="dark"] {
  --bg: #121619;
  --panel: #1a2025;
  --panel-2: #20272d;
  --border: #2e3840;
  --fg: #e6edf3;
  --muted: #9aa7b2;
  --accent: #62c7f2;
  --accent-soft: #16323f;
  --warn: #e2a24c;
  --warn-soft: #3a2a13;
  --ok: #7bd39f;
  --code-bg: #141a1e;
  --shadow: 0 1px 2px rgba(0, 0, 0, .35), 0 8px 24px rgba(0, 0, 0, .3);
}
* { box-sizing: border-box; }
@media (prefers-reduced-motion: no-preference) {
  html { scroll-behavior: smooth; }
}
body {
  margin: 0;
  background: var(--bg);
  color: var(--fg);
  font: 15px/1.6 system-ui, -apple-system, "Segoe UI", Roboto,
        "Noto Sans SC", "PingFang SC", "Microsoft YaHei", sans-serif;
}
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
a:focus-visible, button:focus-visible, input:focus-visible, summary:focus-visible {
  outline: 2px solid var(--accent);
  outline-offset: 2px;
}
code, pre, .mono {
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas,
               "Liberation Mono", monospace;
}
.layout { display: flex; min-height: 100vh; }
.sidebar {
  width: 310px;
  flex: 0 0 310px;
  background: var(--panel);
  border-right: 1px solid var(--border);
  height: 100vh;
  position: sticky;
  top: 0;
  display: flex;
  flex-direction: column;
}
.brand { padding: 18px 18px 10px; border-bottom: 1px solid var(--border); }
.brand h1 { font-size: 16px; margin: 0 0 2px; letter-spacing: .2px; }
.brand .sub { color: var(--muted); font-size: 12px; }
.search-wrap { padding: 12px 14px 8px; position: relative; }
.search-wrap input {
  width: 100%;
  padding: 9px 10px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--panel-2);
  color: var(--fg);
  font-size: 13px;
  outline: none;
}
.search-wrap input:focus { border-color: var(--accent); box-shadow: 0 0 0 3px var(--accent-soft); }
.search-results {
  position: absolute;
  left: 14px; right: 14px; top: 52px;
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: 8px;
  box-shadow: var(--shadow);
  max-height: 50vh;
  overflow: auto;
  z-index: 30;
  display: none;
}
.search-results.show { display: block; }
.sr-item { display: block; padding: 7px 10px; border-bottom: 1px solid var(--border); color: var(--fg); }
.sr-item:last-child { border-bottom: 0; }
.sr-item:hover, .sr-item.active { background: var(--accent-soft); text-decoration: none; }
.sr-item .n { font-family: ui-monospace, monospace; font-size: 12.5px; color: var(--accent); }
.sr-item .p { display: block; font-size: 11px; color: var(--muted); }
.sr-empty { padding: 10px; color: var(--muted); font-size: 12px; }
.nav { overflow: auto; padding: 4px 10px 24px; flex: 1; }
.nav .group { margin-top: 12px; }
.nav .group-title {
  font-size: 11px; letter-spacing: .09em; text-transform: uppercase;
  color: var(--muted); padding: 4px 8px;
}
.nav a.mod {
  display: flex; justify-content: space-between; gap: 8px;
  padding: 5px 8px; border-radius: 6px; color: var(--fg);
  font-size: 12.5px; line-height: 1.35;
}
.nav a.mod:hover { background: var(--panel-2); text-decoration: none; }
.nav a.mod.active { background: var(--accent-soft); color: var(--accent); }
.nav .count { color: var(--muted); font-size: 11px; flex: 0 0 auto; }
.main { flex: 1; min-width: 0; padding: 34px 46px 80px; }
.main-inner { max-width: 1080px; margin: 0 auto; }
.topline { display: flex; align-items: center; gap: 10px; margin-bottom: 8px; }
.topline .crumb { color: var(--muted); font-size: 12.5px; }
.spacer { flex: 1; }
.btn {
  border: 1px solid var(--border); background: var(--panel); color: var(--fg);
  border-radius: 8px; padding: 6px 10px; cursor: pointer; font-size: 12.5px;
}
.btn:hover { border-color: var(--accent); color: var(--accent); }
h1.page-title { font-size: 28px; line-height: 1.25; margin: 6px 0 4px; letter-spacing: -.3px; }
h2 { font-size: 20px; margin: 34px 0 10px; letter-spacing: -.2px; }
h3 { font-size: 16px; margin: 22px 0 8px; }
.subtitle { color: var(--muted); margin: 0 0 24px; }
.stats { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 12px; margin: 20px 0 8px; }
.stat {
  background: var(--panel); border: 1px solid var(--border);
  border-radius: 12px; padding: 14px 16px; box-shadow: var(--shadow);
}
.stat .k { color: var(--muted); font-size: 12px; }
.stat .v { font-size: 24px; font-weight: 650; margin-top: 2px; letter-spacing: -.3px; }
.stat .v small { font-size: 13px; color: var(--muted); font-weight: 400; }
table { border-collapse: collapse; width: 100%; margin: 12px 0 20px; font-size: 13.5px; }
th, td {
  text-align: left; padding: 8px 10px; border-bottom: 1px solid var(--border);
  vertical-align: top; overflow-wrap: anywhere;
}
th { color: var(--muted); font-weight: 600; font-size: 12px; text-transform: uppercase; letter-spacing: .04em; }
.table-wrap { max-width: 100%; overflow-x: auto; }
td.num, th.num { text-align: right; font-variant-numeric: tabular-nums; }
.bar { height: 6px; background: var(--panel-2); border-radius: 99px; overflow: hidden; min-width: 80px; }
.bar > i { display: block; height: 100%; background: var(--accent); border-radius: 99px; }
.module-head {
  border-bottom: 1px solid var(--border); padding-bottom: 18px; margin-bottom: 20px;
}
.module-head .path { font-family: ui-monospace, monospace; font-size: 12.5px; color: var(--muted); }
.module-desc { color: var(--fg); max-width: 78ch; }
.fn {
  background: var(--panel); border: 1px solid var(--border);
  border-left: 3px solid var(--accent);
  border-radius: 12px; padding: 18px 20px; margin: 16px 0;
  box-shadow: var(--shadow); scroll-margin-top: 18px;
}
.fn.undoc { border-left-color: var(--warn); }
.fn-head { display: flex; align-items: baseline; gap: 10px; flex-wrap: wrap; }
.fn-name { font-family: ui-monospace, monospace; font-size: 16px; font-weight: 650; }
.badge {
  font-size: 10.5px; padding: 2px 7px; border-radius: 99px;
  border: 1px solid var(--border); color: var(--muted); white-space: nowrap;
}
.badge.auto { color: var(--warn); border-color: var(--warn); background: var(--warn-soft); }
.badge.undocb { color: var(--warn); border-color: var(--warn); background: var(--warn-soft); }
.badge.static { color: var(--accent); border-color: var(--accent); background: var(--accent-soft); }
.sig {
  background: var(--code-bg); border: 1px solid var(--border);
  border-radius: 8px; padding: 11px 13px; margin: 12px 0;
  font-family: ui-monospace, monospace; font-size: 13px;
  overflow-x: auto; white-space: pre; color: var(--fg);
}
.doc p { margin: 8px 0; max-width: 82ch; overflow-wrap: anywhere; }
.doc ul { margin: 8px 0 8px 20px; padding: 0; }
.doc code { background: var(--code-bg); border: 1px solid var(--border); border-radius: 4px; padding: 0 4px; font-size: 12.5px; }
.doc table { margin: 10px 0; }
.note { border-left: 3px solid var(--warn); background: var(--warn-soft); padding: 9px 12px; border-radius: 0 8px 8px 0; margin: 10px 0; font-size: 13.5px; }
.body-notes { margin: 14px 0 0; border: 1px solid var(--border); border-radius: 8px; background: var(--panel-2); }
.body-notes summary { cursor: pointer; padding: 9px 12px; color: var(--muted); font-size: 13px; user-select: none; }
.body-notes summary:hover { color: var(--accent); }
.body-notes[open] summary { border-bottom: 1px solid var(--border); }
.notes-list { padding: 2px 14px 10px; }
.body-note { padding: 9px 0; border-top: 1px dashed var(--border); }
.body-note:first-child { border-top: 0; }
.note-line { font-family: ui-monospace, monospace; font-size: 11px; color: var(--muted); margin-bottom: 3px; }
.note-text { font-size: 13.5px; overflow-wrap: anywhere; }
.note-text p { margin: 4px 0; }
.note-text ul { margin: 4px 0 4px 18px; padding: 0; }
.source { color: var(--muted); font-size: 12px; margin-top: 12px; font-family: ui-monospace, monospace; overflow-wrap: anywhere; }
.muted { color: var(--muted); }
.empty { color: var(--muted); padding: 24px 0; }
.overview-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 18px; }
.card { background: var(--panel); border: 1px solid var(--border); border-radius: 12px; padding: 16px 18px; box-shadow: var(--shadow); }
.card h3 { margin: 0 0 10px; }
.kv { display: flex; justify-content: space-between; gap: 12px; padding: 4px 0; font-size: 13px; }
.kv span:last-child { color: var(--muted); }
pre.sample { background: var(--code-bg); border: 1px solid var(--border); border-radius: 8px; padding: 12px; overflow-x: auto; font-size: 12.5px; line-height: 1.55; }
@media (max-width: 980px) {
  .stats { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .overview-grid { grid-template-columns: 1fr; }
  .main { padding: 26px 22px 70px; }
}
@media (max-width: 760px) {
  .layout { display: block; }
  .sidebar { width: auto; height: auto; position: static; border-right: 0; border-bottom: 1px solid var(--border); }
  .nav { max-height: 280px; }
  .main { padding: 22px 16px 60px; }
}
</style>
<div class="layout">
  <aside class="sidebar">
    <div class="brand">
      <h1>Avatar OS · C API</h1>
      <div class="sub" id="brand-sub">正在载入…</div>
    </div>
    <div class="search-wrap">
      <input id="search" type="search" aria-label="搜索函数、模块或说明" placeholder="搜索函数 / 模块 / 说明…  ( / )" autocomplete="off" spellcheck="false">
      <div class="search-results" id="search-results"></div>
    </div>
    <nav class="nav" id="nav"></nav>
  </aside>
  <main class="main">
    <div class="main-inner" id="content"></div>
  </main>
</div>
<script>
const DATA = __API_DATA__;
const byPath = new Map(DATA.modules.map(m => [m.path, m]));
const $ = (id) => document.getElementById(id);

function esc(s) {
  return String(s == null ? "" : s)
    .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;").replace(/'/g, "&#39;");
}
function inlineMd(s) {
  let x = esc(s);
  x = x.replace(/`([^`]+)`/g, "<code>$1</code>");
  x = x.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
  return x;
}
function mdToHtml(text) {
  if (!text) return "";
  const blocks = String(text).replace(/\r\n/g, "\n").split(/\n{2,}/);
  let out = "";
  for (const block of blocks) {
    const lines = block.split("\n").map(l => l.trim()).filter(l => l.length);
    if (!lines.length) continue;
    if (lines.every(l => /^[-*]\s+/.test(l))) {
      out += "<ul>" + lines.map(l => "<li>" + inlineMd(l.replace(/^[-*]\s+/, "")) + "</li>").join("") + "</ul>";
    } else {
      out += "<p>" + lines.map(inlineMd).join("<br>") + "</p>";
    }
  }
  return out;
}
function pct(n, d) { return d ? (100 * n / d).toFixed(1) + "%" : "0%"; }
function fnDoc(f) {
  const d = f.doc;
  if (!d) return "";
  let h = '<div class="doc">';
  if (d.description) h += mdToHtml(d.description);
  else if (d.brief) h += mdToHtml(d.brief);
  if (f.params && f.params.length && f.params.some(p => d.params && d.params[p.name])) {
    h += '<div class="table-wrap"><table><thead><tr><th>参数</th><th>类型</th><th>说明</th></tr></thead><tbody>';
    for (const p of f.params) {
      if (p.name === "...") { h += "<tr><td><code>...</code></td><td>可变参数</td><td>—</td></tr>"; continue; }
      const pd = (d.params && d.params[p.name]) || "—";
      h += "<tr><td><code>" + esc(p.name || "—") + "</code></td><td><code>" +
           esc(p.type) + "</code></td><td>" + inlineMd(pd) + "</td></tr>";
    }
    h += "</tbody></table></div>";
  }
  if (d.returns) h += "<p><strong>返回</strong>：" + inlineMd(d.returns) + "</p>";
  for (const n of (d.notes || [])) {
    const label = {note:"注意", warning:"警告", see:"参见", deprecated:"已废弃", since:"引入版本", todo:"待办"}[n.kind] || n.kind;
    h += '<div class="note"><strong>' + esc(label) + "</strong>：" + inlineMd(n.text) + "</div>";
  }
  h += "</div>";
  return h;
}
function fnBodyNotes(f) {
  const notes = f.body_notes || [];
  if (!notes.length) return "";
  let h = '<details class="body-notes"><summary>函数内备注 · ' + notes.length +
          ' 条</summary><div class="notes-list">';
  for (const n of notes) {
    h += '<div class="body-note"><div class="note-line">第 ' + n.line + ' 行' +
         (n.inline ? '（行尾）' : '') + '</div><div class="note-text">' +
         mdToHtml(n.text) + '</div></div>';
  }
  h += "</div></details>";
  return h;
}
function renderSidebar() {
  const groups = new Map();
  for (const m of DATA.modules) {
    if (!groups.has(m.subsystem)) groups.set(m.subsystem, []);
    groups.get(m.subsystem).push(m);
  }
  const order = ["kernel","driver","fs","lib","boot","platforms","apps","include","tools"];
  const keys = [...groups.keys()].sort((a,b) => {
    const ia = order.indexOf(a), ib = order.indexOf(b);
    return (ia < 0 ? 99 : ia) - (ib < 0 ? 99 : ib) || a.localeCompare(b);
  });
  let h = "";
  for (const k of keys) {
    const mods = groups.get(k).sort((a,b) => a.path.localeCompare(b.path));
    const nfn = mods.reduce((n,m) => n + m.functions.length, 0);
    h += '<div class="group"><div class="group-title">' + esc(k) + " · " + nfn + "</div>";
    for (const m of mods) {
      h += '<a class="mod" href="#/m/' + encodeURIComponent(m.path) + '" data-path="' +
           esc(m.path) + '"><span>' + esc(m.path.split("/").pop()) +
           (m.title ? ' <span class="muted" style="font-size:11px">· ' + esc(m.title) + "</span>" : "") +
           '</span><span class="count">' + m.functions.length + "</span></a>";
    }
    h += "</div>";
  }
  $("nav").innerHTML = h;
  $("brand-sub").textContent = DATA.stats.modules_with_api + " 个模块 · " +
    DATA.stats.functions_total + " 个函数 · 说明覆盖 " + DATA.stats.coverage.toFixed(1) + "%";
}
function renderOverview() {
  const s = DATA.stats;
  let h = '<div class="topline"><span class="crumb">总览</span><span class="spacer"></span>' +
          '<button class="btn" id="theme-btn" aria-label="切换明暗主题" title="切换明暗主题">◐ 主题</button></div>';
  h += '<h1 class="page-title">Avatar OS C API 参考</h1>';
  h += '<p class="subtitle">生成于 ' + esc(DATA.generated) + " · 由 <code>make apidoc</code> 从源码注释生成 · " +
       "Doxygen 兼容注释 + 现有中文块注释</p>";
  h += '<div class="stats">' +
    '<div class="stat"><div class="k">C 模块</div><div class="v">' + s.modules_total + ' <small>个源文件</small></div></div>' +
    '<div class="stat"><div class="k">函数</div><div class="v">' + s.functions_total + '</div></div>' +
    '<div class="stat"><div class="k">有说明</div><div class="v">' + s.documented + ' <small>(' + s.coverage.toFixed(1) + '%)</small></div>' +
    '<div class="muted" style="font-size:11px;margin-top:3px">人工 ' + s.manual + ' · 自动 ' + s.auto + '</div></div>' +
    '<div class="stat"><div class="k">代码规模</div><div class="v">' + (s.loc/1000).toFixed(1) + 'k <small>行</small></div></div>' +
    "</div>";
  h += '<h2>子系统</h2><div class="table-wrap"><table><thead><tr><th>子系统</th><th class="num">模块</th><th class="num">函数</th><th class="num">人工</th><th class="num">自动</th><th>说明覆盖率</th><th>入口</th></tr></thead><tbody>';
  const sorted = [...s.subsystems].sort((a,b) => b.functions - a.functions);
  for (const x of sorted) {
    const cov = x.coverage;
    h += "<tr><td><strong>" + esc(x.name) + "</strong></td><td class='num'>" + x.modules +
         "</td><td class='num'>" + x.functions + "</td><td class='num'>" + (x.documented - x.auto) +
         "</td><td class='num'>" + x.auto +
         "</td><td><div style='display:flex;align-items:center;gap:8px'><div class='bar' style='flex:1'><i style='width:" +
         cov + "%'></i></div><span class='muted' style='font-size:12px'>" + cov.toFixed(1) +
         "%</span></div></td><td>" + (x.functions ? '<a href="#/s/' + encodeURIComponent(x.name) + '">浏览</a>' : "—") +
         "</td></tr>";
  }
  h += "</tbody></table></div>";
  h += '<div class="overview-grid"><div class="card"><h3>注释怎么写</h3>' +
       '<pre class="sample">/**\n * @brief 从等待队列里摘掉一个任务。\n * @param queue 目标等待队列。\n * @return 被摘下的任务；为空返回 NULL。\n * @note 调用者必须持有 queue-&gt;lock。\n */\ntask_t *waitq_pop(wait_queue_t *queue);</pre>' +
       '<p class="muted" style="font-size:13px">注释紧挨函数定义上方。装饰性的 <code>/* ── name ── */</code> 会被跳过；它上方的正文注释会被收录。</p></div>' +
       '<div class="card"><h3>覆盖情况</h3>' +
       '<div class="kv"><span>人工注释</span><span>' + s.manual + '</span></div>' +
       '<div class="kv"><span>自动摘要</span><span>' + s.auto + '</span></div>' +
       '<div class="kv"><span>函数内备注</span><span>' + s.body_notes + ' 条</span></div>' +
       '<div class="kv"><span>尚无说明</span><span>' + s.undocumented + '</span></div>' +
       '<div class="kv"><span>说明覆盖率（人工 + 自动）</span><span>' + s.coverage.toFixed(1) + '%</span></div>' +
       '<p class="muted" style="font-size:13px;margin-top:12px">运行 <code>make apidoc</code> 重新生成；<code>make apidoc-check</code> 在 CI 里检查提取警告。</p></div></div>';
  h += renderAllSubsystems();
  $("content").innerHTML = h;
  bindTheme();
  document.title = "Avatar OS · C API 参考";
  window.scrollTo(0, 0);
  markActive(null);
}
function renderAllSubsystems() {
  let h = '<h2>全部模块</h2>';
  const order = ["kernel","driver","fs","lib","boot","platforms","apps","include","tools"];
  const mods = [...DATA.modules].sort((a,b) => {
    const ia = order.indexOf(a.subsystem), ib = order.indexOf(b.subsystem);
    return (ia < 0 ? 99 : ia) - (ib < 0 ? 99 : ib) || a.path.localeCompare(b.path);
  });
  for (const m of mods) {
    h += '<div style="padding:10px 0;border-bottom:1px solid var(--border)"><a href="#/m/' +
         encodeURIComponent(m.path) + '"><code>' + esc(m.path) + "</code></a>" +
         (m.title ? ' <span class="muted">— ' + esc(m.title) + "</span>" : "") +
         '<span class="muted" style="float:right;font-size:12px">' + m.functions.length + " 个函数</span></div>";
  }
  return h;
}
function renderModule(path, focusLine) {
  const m = byPath.get(path);
  if (!m) { renderOverview(); return; }
  const documented = m.functions.filter(f => f.doc).length;
  let h = '<div class="topline"><span class="crumb"><a href="#/">总览</a> / ' +
          esc(m.subsystem) + "</span><span class='spacer'></span>" +
          '<button class="btn" id="theme-btn" aria-label="切换明暗主题" title="切换明暗主题">◐ 主题</button></div>';
  h += '<div class="module-head"><h1 class="page-title">' + esc(m.title || m.path.split("/").pop()) + "</h1>" +
       '<div class="path">' + esc(m.path) + " · " + m.functions.length + " 个函数 · " +
       documented + " 个有说明 · " + m.loc + " 行</div>" +
       (m.description ? '<div class="module-desc" style="margin-top:12px">' + mdToHtml(m.description) + "</div>" : "") +
       "</div>";
  for (const f of m.functions) {
    const undoc = !f.doc;
    h += '<section class="fn' + (undoc ? " undoc" : "") + '" id="fn-' + f.line + '">';
    h += '<div class="fn-head"><span class="fn-name">' + esc(f.name) + "</span>";
    for (const l of (f.linkage || [])) h += '<span class="badge static">' + esc(l) + "</span>";
    if (f.auto) h += '<span class="badge auto" title="来自 docs/api/auto_summaries.json，不是源码中的人工注释">自动摘要</span>';
    if (undoc) h += '<span class="badge undocb">无文档注释</span>';
    h += "</div>";
    h += '<div class="sig">' + esc(f.signature) + "</div>";
    if (undoc) {
      h += '<div class="note">尚无文档注释。按 <code>docs/api/README.md</code> 的格式在函数上方补充后，重新运行 <code>make apidoc</code>。</div>';
    } else {
      h += fnDoc(f);
    }
    h += fnBodyNotes(f);
    h += '<div class="source">' + esc(m.path) + ":" + f.line + "</div></section>";
  }
  $("content").innerHTML = h;
  bindTheme();
  document.title = (m.title || m.path) + " · Avatar OS C API";
  markActive(path);
  if (focusLine) {
    const el = $("fn-" + focusLine);
    if (el) { el.scrollIntoView({block: "start"}); }
  } else {
    window.scrollTo(0, 0);
  }
}
function renderSubsystem(name) {
  const mods = DATA.modules.filter(m => m.subsystem === name);
  const nfn = mods.reduce((n,m) => n + m.functions.length, 0);
  let h = '<div class="topline"><span class="crumb"><a href="#/">总览</a> / ' + esc(name) +
          "</span><span class='spacer'></span><button class='btn' id='theme-btn' aria-label='切换明暗主题' title='切换明暗主题'>◐ 主题</button></div>";
  h += '<h1 class="page-title">' + esc(name) + " — C API</h1>";
  h += '<p class="subtitle">' + mods.length + " 个模块 · " + nfn + " 个函数</p>";
  h += '<div class="overview-grid">';
  for (const m of mods.sort((a,b) => a.path.localeCompare(b.path))) {
    h += '<div class="card"><h3><a href="#/m/' + encodeURIComponent(m.path) + '">' +
         esc(m.title || m.path.split("/").pop()) + "</a></h3><div class='path muted' style='font-size:12px;font-family:ui-monospace,monospace'>" +
         esc(m.path) + "</div><div class='muted' style='font-size:13px;margin-top:6px'>" +
         m.functions.length + " 个函数 · " + m.functions.filter(f=>f.doc).length + " 个有说明</div></div>";
  }
  h += "</div>";
  $("content").innerHTML = h;
  bindTheme();
  document.title = name + " · Avatar OS C API";
  window.scrollTo(0, 0);
  markActive(null);
}
function markActive(path) {
  for (const a of document.querySelectorAll(".nav a.mod")) {
    a.classList.toggle("active", a.dataset.path === path);
  }
}
function bindTheme() {
  const b = $("theme-btn");
  if (b) b.onclick = () => {
    const cur = document.documentElement.getAttribute("data-theme");
    const next = cur === "dark" ? "light" : cur === "light" ? "" : "dark";
    if (next) document.documentElement.setAttribute("data-theme", next);
    else document.documentElement.removeAttribute("data-theme");
  };
}
function route() {
  const h = location.hash.slice(1);
  const parts = h.split("/").filter(Boolean).map(decodeURIComponent);
  if (parts[0] === "m" && parts[1]) renderModule(parts[1], parts[2] ? parseInt(parts[2], 10) : null);
  else if (parts[0] === "s" && parts[1]) renderSubsystem(parts[1]);
  else renderOverview();
}
function setupSearch() {
  const input = $("search"), box = $("search-results");
  const index = [];
  for (const m of DATA.modules) {
    for (const f of m.functions) {
      index.push({
        name: f.name, path: m.path, line: f.line,
        brief: (f.doc && (f.doc.brief || f.doc.description)) || "",
        hay: (f.name + " " + m.path + " " + ((f.doc && (f.doc.brief || f.doc.description)) || "")).toLowerCase()
      });
    }
  }
  let active = -1, results = [];
  function close() { box.classList.remove("show"); box.innerHTML = ""; active = -1; }
  function draw() {
    if (!results.length) { box.innerHTML = '<div class="sr-empty">没有匹配项</div>'; box.classList.add("show"); return; }
    box.innerHTML = results.map((r, i) =>
      '<a class="sr-item' + (i === active ? " active" : "") + '" href="#/m/' +
      encodeURIComponent(r.path) + "/" + r.line + '"><span class="n">' + esc(r.name) +
      '</span><span class="p">' + esc(r.path) + '</span></a>').join("");
    box.classList.add("show");
  }
  input.addEventListener("input", () => {
    const q = input.value.trim().toLowerCase();
    if (!q) { close(); return; }
    results = index.filter(r => r.hay.includes(q)).slice(0, 60);
    active = results.length ? 0 : -1;
    draw();
  });
  input.addEventListener("keydown", (e) => {
    if (e.key === "Escape") { input.value = ""; close(); input.blur(); return; }
    if (!results.length) return;
    if (e.key === "ArrowDown") { e.preventDefault(); active = (active + 1) % results.length; draw(); }
    else if (e.key === "ArrowUp") { e.preventDefault(); active = (active - 1 + results.length) % results.length; draw(); }
    else if (e.key === "Enter" && active >= 0) {
      e.preventDefault();
      const r = results[active];
      input.value = ""; close();
      location.hash = "#/m/" + encodeURIComponent(r.path) + "/" + r.line;
    }
  });
  box.addEventListener("click", () => { input.value = ""; close(); });
  document.addEventListener("keydown", (e) => {
    if (e.key === "/" && document.activeElement !== input &&
        !/^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName)) {
      e.preventDefault(); input.focus();
    }
  });
  document.addEventListener("click", (e) => {
    if (!box.contains(e.target) && e.target !== input) close();
  });
}
renderSidebar();
setupSearch();
window.addEventListener("hashchange", route);
route();
</script>
"""


# ─── 主流程 ──────────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="从 C 源码注释生成 API 参考文档（类 rustdoc）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--out", default=None, help="输出目录（默认 <root>/docs/api）")
    ap.add_argument("--format", choices=["all", "md", "html", "json"], default="all")
    ap.add_argument("--check", action="store_true", help="只检查，不写文件")
    ap.add_argument("--min-coverage", type=float, default=None,
                    help="配合 --check：注释覆盖率低于该百分比时失败")
    ap.add_argument("--include-tests", action="store_true", help="同时扫描 tests/unit-test")
    ap.add_argument("--strict", action="store_true", help="有提取警告时也失败")
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root)
    out_dir = os.path.abspath(args.out or os.path.join(root, "docs", "api"))

    modules, warnings = build_docs(root, args.include_tests)
    stats = compute_stats(modules)

    print(
        f"[apidoc] 模块 {stats['modules_total']}（含函数 {stats['modules_with_api']}）· "
        f"函数 {stats['functions_total']} · 有说明 {stats['documented']} "
        f"({stats['coverage']:.1f}%) · 人工 {stats['manual']} · 自动 {stats['auto']}"
    )
    for s in stats["subsystems"]:
        print(
            f"  {s['name']:<10} 模块 {s['modules']:>3}  函数 {s['functions']:>4}  "
            f"有说明 {s['documented']:>4}  人工 {s['documented'] - s['auto']:>4}  "
            f"自动 {s['auto']:>4}  {s['coverage']:>5.1f}%"
        )
    if warnings:
        print(f"[apidoc] {len(warnings)} 条提取警告：", file=sys.stderr)
        for w in warnings[:30]:
            print("  " + w, file=sys.stderr)
        if len(warnings) > 30:
            print(f"  ... 还有 {len(warnings) - 30} 条", file=sys.stderr)

    if args.check:
        ok = True
        if args.min_coverage is not None and stats["coverage"] < args.min_coverage:
            print(
                f"[apidoc] 覆盖率 {stats['coverage']:.1f}% 低于阈值 "
                f"{args.min_coverage:.1f}%",
                file=sys.stderr,
            )
            ok = False
        if args.strict and warnings:
            ok = False
        return 0 if ok else 1

    os.makedirs(out_dir, exist_ok=True)

    if args.format in ("all", "md"):
        md_files = render_markdown(modules, stats)
        for name, content in md_files.items():
            with open(os.path.join(out_dir, name), "w", encoding="utf-8") as f:
                f.write(content)
        print(f"[apidoc] 写入 {len(md_files)} 个 Markdown 文件 → {os.path.relpath(out_dir, root)}/")

    if args.format in ("all", "json"):
        api = {
            "project": "Avatar OS",
            "generated": datetime.date.today().isoformat(),
            "stats": stats,
            "modules": modules,
        }
        with open(os.path.join(out_dir, "api.json"), "w", encoding="utf-8") as f:
            json.dump(api, f, ensure_ascii=False, separators=(",", ":"))
        print(f"[apidoc] 写入 api.json")

    if args.format in ("all", "html"):
        html_dir = os.path.join(out_dir, "html")
        os.makedirs(html_dir, exist_ok=True)
        page = render_html(modules, stats)
        with open(os.path.join(html_dir, "index.html"), "w", encoding="utf-8") as f:
            f.write(page)
        print(
            f"[apidoc] 写入 HTML → "
            f"{os.path.relpath(os.path.join(html_dir, 'index.html'), root)}"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
