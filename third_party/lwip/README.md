# lwIP (Avatar OS 内嵌副本)

本目录**不再是 git submodule**，而是直接由本仓库维护的源码副本
（与 `third_party/lwext4/` 同一种做法）。

## 来源

| | |
|---|---|
| 上游 | https://git.savannah.nongnu.org/git/lwip.git |
| 版本 | `STABLE-2_2_0_RELEASE`，commit `0a0452b2c39bdd91e252aef045c115f88f6ca773` |
| 是否 fork | **否**。这是**未经修改的上游代码**，我们一个字节都没改。 |

本目录 = 上游的 **`src/` + `COPYING`**。其余（`contrib/` `test/` `doc/`、
`CHANGELOG` `UPGRADING` `README` `FILES` `FEATURES` `BUILDING`
`CMakeLists.txt`、codespell 脚本、`.github/` `.vscode/`）已删除 ——
本内核的构建只引用 `src/` 和 `-I$(LWIP_DIR)/src/include`。

> ⚠️ 因此**上游的构建体系在本副本里不可用**。要跑 lwIP 自带的测试或
> 用它的 CMake 构建，请用上游仓库。

## 许可：BSD-3-Clause（宽松）

`COPYING` 是 SICS（瑞典计算机科学院）2001/2002 的那份三条款 BSD：

```
Copyright (c) 2001, 2002 Swedish Institute of Computer Science.
All rights reserved.
1. 源码分发须保留版权声明与免责声明
2. 二进制分发须在文档中复现版权声明与免责声明
3. 不得用作者名义为衍生产品背书
```

**不是 GPL。** 关键区别在于 BSD-3 不要求你开源自己的代码 —— 分发内核镜像时
只需保留版权与免责声明即可，不必按任何特定许可开放本项目。

上游树里曾有**唯一**一个 GPL 文件：

```
contrib/apps/LwipMibCompiler/SharpSnmpLib/license.txt
```

那是 contrib 里一个 .NET MIB 编译工具，从不参与本内核构建；`contrib/` 已随
本次裁剪整体删除，所以本目录现在是**干净的纯 BSD-3-Clause**。

**校验方法**（换机器/换人接手时先跑一遍）：

```bash
cd third_party/lwip && grep -rl "GNU General Public License" src/ COPYING
# 应无输出
```

## 为什么 lwIP 保持"未修改的上游代码"很重要

`third_party/lwext4/` 之所以也被内嵌，是因为我们 **fork 并修改了**它 ——
而 lwIP 没有。这个区别有实际后果：

- **升级**：将来要升到新的 lwIP release，直接把上游 `src/` 覆盖过来即可，
  没有本地改动要合并。
- **不要在这个目录里做项目相关的改动**。本项目的适配都在
  **`kernel/net/lwip_port/`**（`netif_avatar.c`、`lwipopts.h`、`sys_arch` 等）
  和 `Makefile` 的 `LWIP_CFLAGS` 里。一旦在这里改了，上面那条"直接覆盖升级"
  就不成立了，而且会重演 lwext4 那个"钉住的 commit 不在配置的远端上"的坑。

## 构建接进来的是什么

`Makefile` 里 `LWIP_CORE_SRCS` 逐条列出用到的源文件（`src/core/`、
`src/core/ipv4/`、`src/netif/`），编译用 `LWIP_CFLAGS`
（`-I$(LWIP_DIR)/src/include` + `-Ikernel/net/lwip_port`，带 `-w`）。

**注意 `src/` 里有一部分没被编进来**（`src/apps/`、`src/api/` 等）——
保留它们是出于"上游源码完整性"的考虑（覆盖升级时不用挑挑拣拣），
不是构建需要。
