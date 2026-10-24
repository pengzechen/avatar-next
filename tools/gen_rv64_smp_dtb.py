#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# tools/gen_rv64_smp_dtb.py — 生成 RISC-V guest 的多核 DTB
#
# 单核基线是 imgs/guests/rv64/linux.dts(手写、带完整注释)。多核版本只有
# 两处不同，其余节点逐字保持一致：
#
#   1) cpus 下 N 个 cpu@i，每个各带自己的 cpuN_intc(riscv,cpu-intc)；
#   2) PLIC 的 interrupts-extended 列出 N 个 S-mode context
#      (<&cpu0_intc 9>, <&cpu1_intc 9>, ...)。
#
# PLIC 的 context 编号 = interrupts-extended 里的顺序 = vCPU 编号，这一点
# 必须与 vplic.c 的「context N == vCPU N」严格对齐；顺序写反的症状是
# 某个核永远收不到外部中断(UART 只能从一个核输入)，而且没有任何报错。
#
# 只生成 N>1 的 linux-smpN.dtb；linux.dtb 仍是手工维护的单核版本，避免
# 生成脚本反过来改掉单核基线的试验田。
#
# 用法：
#   tools/gen_rv64_smp_dtb.py            # 生成 2/4/8/12 四份
#   tools/gen_rv64_smp_dtb.py 2 4        # 只生成指定档位
#
# 需要 host 安装 device-tree-compiler(dtc)。产物已签入仓库；DTB 里的
# 注释/格式无关紧要，改完请连同本脚本一起提交。

import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUTDIR = ROOT / "imgs" / "guests" / "rv64"
SUPPORTED = (2, 4, 8, 12)

TEMPLATE = """\
/dts-v1/;

/*
 * Avatar OS — RISC-V 64 guest Linux 设备树（{n} vCPU，自动生成）
 *
 * !!! 本文件由 tools/gen_rv64_smp_dtb.py 生成，不要手改 !!!
 * 单核基线见 linux.dts；改设备/属性请改脚本并重新生成，否则下次生成会覆盖。
 *
 * PLIC 的 interrupts-extended 顺序 == context 编号 == vCPU 编号，
 * 与 kernel/vmm/vdev/vplic.c 的「context N == vCPU N」严格对应。
 */
/ {{
	#address-cells = <2>;
	#size-cells = <2>;
	compatible = "avatar,riscv64-virt";
	model = "Avatar OS RISC-V virtual machine ({n} vCPUs)";

	chosen {{
		bootargs = "console=ttyS0 earlycon=uart8250,mmio,0x10000000 rdinit=/init panic_on_warn=0 oops=panic";
		linux,initrd-start = <0x0 0x0>;
		linux,initrd-end = <0x0 0x0>;
		stdout-path = "/soc/uart@10000000";
	}};

	memory@a0000000 {{
		device_type = "memory";
		reg = <0x0 0xa0000000 0x0 0x0c000000>;
	}};

	cpus {{
		#address-cells = <1>;
		#size-cells = <0>;
		timebase-frequency = <10000000>;
{cpus}
	}};

	soc {{
		#address-cells = <2>;
		#size-cells = <2>;
		compatible = "simple-bus";
		ranges;

		plic: interrupt-controller@c000000 {{
			#address-cells = <0>;
			#interrupt-cells = <1>;
			compatible = "riscv,plic0";
			interrupt-controller;
			interrupts-extended = {plic_ctx};
			reg = <0x0 0x0c000000 0x0 0x00400000>;
			riscv,ndev = <63>;
		}};

		uart@10000000 {{
			compatible = "ns16550a";
			reg = <0x0 0x10000000 0x0 0x100>;
			clock-frequency = <3686400>;
			current-speed = <115200>;
			reg-shift = <0>;
			reg-io-width = <1>;
			interrupt-parent = <&plic>;
			interrupts = <10>;
		}};
	}};
}};
"""

CPU_TEMPLATE = """\
		cpu@{i} {{
			device_type = "cpu";
			reg = <{i}>;
			status = "okay";
			compatible = "riscv";
			riscv,isa = "rv64imafdcsu";
			mmu-type = "riscv,sv39";

			cpu{i}_intc: interrupt-controller {{
				#interrupt-cells = <1>;
				interrupt-controller;
				compatible = "riscv,cpu-intc";
			}};
		}};
"""


def gen_one(n: int) -> int:
    cpus = "\n".join(CPU_TEMPLATE.format(i=i) for i in range(n))
    plic_ctx = ", ".join(f"<&cpu{i}_intc 9>" for i in range(n))
    dts = TEMPLATE.format(n=n, cpus=cpus, plic_ctx=plic_ctx)

    out = OUTDIR / f"linux-smp{n}.dtb"
    with tempfile.NamedTemporaryFile("w", suffix=".dts", delete=False) as f:
        f.write(dts)
        tmp_dts = pathlib.Path(f.name)
    try:
        subprocess.run(
            ["dtc", "-I", "dts", "-O", "dtb", "-o", str(out), str(tmp_dts)],
            check=True,
        )
    finally:
        tmp_dts.unlink(missing_ok=True)

    # 自检：回读 dtb，确认 cpu 节点数与 PLIC context 数都对得上。
    # 生成脚本自己写出坏 DTB 的话，症状会跑到 guest 启动阶段(CPU_ON 一个
    # 不存在的 hart)，离这里很远。
    back = subprocess.run(
        ["dtc", "-I", "dtb", "-O", "dts", str(out)],
        check=True, capture_output=True, text=True,
    ).stdout
    got_cpus = back.count("cpu@")
    got_props = back.count("device_type = \"cpu\"")
    if got_cpus != n or got_props != n:
        raise SystemExit(f"{out}: expected {n} CPUs, got node={got_cpus} prop={got_props}")
    if back.count("interrupts-extended") != 1 or back.count("cpu-intc") != n:
        raise SystemExit(f"{out}: PLIC context count mismatch (expected {n})")

    print(f"  {out.relative_to(ROOT)}  ({n} CPUs, {out.stat().st_size} bytes)")
    return 0


def main(argv) -> int:
    if shutil.which("dtc") is None:
        print("gen_rv64_smp_dtb.py: dtc not found (install device-tree-compiler)",
              file=sys.stderr)
        return 1

    targets = [int(a) for a in argv[1:]] or list(SUPPORTED)
    bad = [n for n in targets if n not in SUPPORTED]
    if bad:
        print(f"unsupported vCPU count(s): {bad}; use {list(SUPPORTED)}",
              file=sys.stderr)
        return 1

    OUTDIR.mkdir(parents=True, exist_ok=True)
    for n in targets:
        gen_one(n)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
