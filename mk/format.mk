# mk/format.mk — clang-format 与 C API 文档目标

# 代码格式化（配置见仓库根的 .clang-format）
#
# FORMAT_DIRS 只列本项目自己的源码树。kernel/net/lwip 是上游代码（lwIP），
# 人家有自己的风格；build/ 是产物。与 PLATFORM 无关。
# fs/ext4/lwext4、fs/fat/fatfs 虽已收编为 first-party，但核心代码保持上游风格，
# 不参与全量格式化（否则和上游 diff 完全失去可比性）；只格式化 port/ 和
# 我们自己的补丁块。
FORMAT_DIRS  := boot driver fs include kernel lib platforms apps tests
# tests/ltp/src 是 `make env` 拉下来的 LTP 上游源码（约 5944 个 .c/.h），
# tests/.artifacts 是跑测例生成的构建/运行目录，apps/rknn-aarch64/include
# 是 RKNN 官方运行时头文件，fs/ext4/lwext4|fs/fat/fatfs 的核心源码保持上游风格 ——
# 都不能格式化（否则 `make format` 会改掉一整个第三方源码树）。
FORMAT_EXCLUDES := tests/ltp/src tests/.artifacts apps/rknn-aarch64/include \
                   apps/dropbear-2024.86 \
                   fs/ext4/lwext4/src fs/ext4/lwext4/include fs/fat/fatfs/source \
                   kernel/net/lwip include/rbtree.h
FORMAT_PRUNE    := $(foreach d,$(FORMAT_EXCLUDES),-path '$(d)' -prune -o)
FORMAT_SRCS     := $(shell find $(FORMAT_DIRS) $(FORMAT_PRUNE) \
    \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null)
CLANG_FORMAT ?= clang-format

# 文件列表必须走文件 + xargs，**不能**把 $(FORMAT_SRCS) 直接展开进命令行：
# 2026-10 已经到 6277 个文件（排除 LTP 后 ~333 个），一次展开超过 ARG_MAX，
# shell 报
#     make: /bin/sh: Argument list too long
# 而 make 只把它显示成 Error 127，看起来像 clang-format 没装。用
# `find -print0` 写一份 NUL 分隔的表，再让 xargs 分批喂给 cat / clang-format。
# 列表只生成一次，保证每遍遍历顺序一致（cksum 的收敛判断依赖这一点）。
FORMAT_FIND := find $(FORMAT_DIRS) $(FORMAT_PRUNE) \
    \( -name '*.c' -o -name '*.h' \) -print0 2>/dev/null

# 为什么是「跑到收敛」而不是「跑一遍」：
# clang-format 不保证幂等，个别构造要两三遍才稳定。本项目实测命中一处 ——
# driver/uart/uart.h 里有个注释插在连续 #define 块中间，那个注释的缩进会在
# 第 1、2 遍之间来回摆一次，第 3 遍才定住。只跑一遍的话，紧接着的
# `make format-check` 会报错，看起来像"格式化没生效"。
format:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || {                     \
	    echo "Cannot find $(CLANG_FORMAT). Install it:"; \
	    echo "  pipx install clang-format     # user-level, no sudo"; \
	    echo "  sudo apt install clang-format";                           \
	    exit 1; }
	@echo "clang-format $$($(CLANG_FORMAT) --version | sed 's/.*version //')" \
	      "- $(words $(FORMAT_SRCS)) file(s)"
	@list=$$(mktemp) || exit 1;                                                \
	trap 'rm -f "$$list"' EXIT;                                                \
	$(FORMAT_FIND) > "$$list";                                                 \
	prev=""; converged="";                                                     \
	for pass in 1 2 3 4 5; do                                                  \
	    cur=$$(xargs -0 -r cat < "$$list" | cksum);                            \
	    if [ "$$cur" = "$$prev" ]; then converged=$$((pass - 1)); break; fi;   \
	    xargs -0 -r $(CLANG_FORMAT) -i < "$$list" || exit 1;                   \
	    prev="$$cur";                                                          \
	done;                                                                      \
	if [ -n "$$converged" ]; then echo "  converged after pass $$converged"; \
	else echo "  WARNING: did not converge after 5 passes; please inspect manually"; fi
	@echo "WARNING: .h files were changed but the build does not track header mtime -"
	@echo "         run make clean before the next build (see the ghost bug section in CLAUDE.md)."

# 注意：--dry-run 只做"再跑一遍会不会变"的判断，所以它隐含要求工作区已经处在
# 不动点上 —— 这正是 format 跑到收敛的原因。CI 里直接用它即可。
format-check:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || {                     \
	    echo "Cannot find $(CLANG_FORMAT) (pipx install clang-format)"; exit 1; }
	@list=$$(mktemp) || exit 1;                                                \
	trap 'rm -f "$$list"' EXIT;                                                \
	$(FORMAT_FIND) > "$$list";                                                 \
	if xargs -0 -r $(CLANG_FORMAT) --dry-run --Werror < "$$list" 2>&1; then    \
	    echo "format check passed ($(words $(FORMAT_SRCS)) files)"; \
	else                                                                       \
	    echo "the files above do not match .clang-format; run make format"; \
	    exit 1;                                                                \
	fi

# ─── C API 文档 ────────────────────────────────────────────────────────────
# 扫描 kernel/driver/fs/lib/boot/platforms/apps/include/tools，取函数定义
# 上方紧挨着的注释（Doxygen 标签或现有中文块注释均可），生成：
#   docs/api/index.md + 每个子系统的 .md + api.json + html/index.html
# 零第三方依赖，不要求安装 Doxygen。注释约定见 docs/api/README.md。
APIDOC := python3 tools/gen_apidoc.py

apidoc:
	@$(APIDOC)

apidoc-check:
	@$(APIDOC) --check --strict
