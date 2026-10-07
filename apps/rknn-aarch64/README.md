# rknn-aarch64 —— RK3588 NPU 的官方用户态运行时

从 `github.com/starry-mix-rk3588/axplat-opi5p` 的 `tools/rknn/` 原样收录。
**二进制，不要手改**；升级就整个替换。

## 为什么不能直接放进 /lib

这套东西是 **glibc** 的：

```
ld-linux-aarch64.so.1   375 KB
libc.so.6               1.4 MB   (GLIBC_2.17 / 2.18 / 2.22 版本化符号)
libm.so.6               633 KB
libpthread.so.0         160 KB
libdl.so.2               14 KB
libgcc_s.so.1            80 KB
libstdc++.so.6          1.8 MB
librga.so               192 KB
librknnrt.so            7.7 MB
rknpu2                  7.3 MB   ← 预编译的 MobileNetV1 demo
```

而本项目的 rootfs 是 **musl** 的（`/lib/ld-musl-aarch64.so.1`）。基镜像里那些
`libm.so.6` / `libpthread.so.0` / `libdl.so.2` 是**指向 musl `libc.so` 的符号
链接**，只为了让 glibc 风格的 `DT_NEEDED` 名字有个落点 —— 真 glibc 二进制
需要版本化符号（`GLIBC_2.17` 等），musl 提供不了，**加载即失败**。

所以这套运行时装在 **`/usr/lib/rknn/`**（独立目录），
`/lib` 一个字节都不动 —— 否则把真 glibc `libm.so.6` 放进去会顶掉那些
符号链接，反而弄坏所有 musl 程序。

## 怎么跑

`rknpu2` 的 PT_INTERP 写死是 `/lib/ld-linux-aarch64.so.1`，但**不用**去满足
它：直接拿自带的 loader 显式启动即可（这样 PT_INTERP 是什么都无所谓）：

```sh
/usr/lib/rknn/ld-linux-aarch64.so.1 --library-path /usr/lib/rknn \
    /usr/lib/rknn/rknpu2 "$@"
```

镜像里已经在 `/bin/rknn` 放了这个包装脚本，所以直接用：

```
~ # rknn
```

## 关于模型

`rknpu2` 是自包含的：`.rodata`（4.77 MiB）里嵌着 RKNN 模型，以及完整的
ImageNet 1000 类标签。所以**不带参数、也不需要外部 `.rknn` 文件**。

模型 blob 的实测边界（`include_bytes!` 内嵌，不是文件）：

```
文件偏移 1,343,848 处 "RKNN" 魔数，长度字段 0x479b00 = 4,692,736 字节 (≈4.48 MiB)
     止于 6,036,584；ImageNet 类别名表起于 6,066,210（"desktop computer" = 528）
```

模型本身：**MobileNetV1**，TFLite 转出，INT8 非对称量化（layer-wise），
`target_platform: ["rk3588"]`；输入 `1×3×224×224` NCHW float32（mean=std=128，
RGB），输出 `1×1001` float32 = ImageNet 1000 类 + 背景。op 列表是 13 组
depthwise/pointwise + 头尾三个，即标准 MobileNetV1 1.0/224。

（这一条是从二进制里推出来的，不是从文档里读到的。万一它其实要模型文件，
`/bin/rknn` 会把参数原样透传，直接 `rknn <模型路径>` 试。）

## 与内核的关系

`librknnrt.so` **实测版本 2.3.2 (429f97ae6b@2025-04-09)**，与
`docs/npu/RK3588_NPU_DRIVER.md` §3 那张反汇编出来的 ABI 表是**同一个版本**
—— 所以那张表对这个 .so 是准的。它要的东西：

```
/dev/dma_heap/*          DMA_HEAP_IOCTL_ALLOC
/dev/dri/renderD128      DRM_IOCTL_VERSION / PRIME_* / RKNPU 六个
DMA_BUF_IOCTL_SYNC       打在 dmabuf fd 上
```

内核侧对应 `DMA=1` + `NPU=rknpu`（`NPU=rknpu` 会自动带上 `DMA=1`）。

---

## YOLOv8 demo（`yolo/`）

`yolo/` 下是 **rknn_model_zoo** 的 YOLOv8 C++ 例子（RK3588 上跑 YOLO 的规范做法）
交叉编译出来的产物，配合 `/bin/yolo` 使用。同样**是二进制，不入库**。

### 怎么编出来的

```bash
# 1) 工具链（必须是 glibc —— librknnrt.so 是 glibc 的，musl 链不出可用的东西）
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu cmake

# 2) 源码
git clone --depth 1 https://github.com/airockchip/rknn_model_zoo.git
cd rknn_model_zoo

# 3) 编译。两个关掉的关键开关：
#    -r  禁用 RGA —— 它要 /dev/rga，本内核没有，用它会在板上直接失败
#    -j  禁用 turbojpeg —— 省一个依赖；代价是 .jpg/.jpeg 读不了（见下）
bash ./build-linux.sh -t rk3588 -a aarch64 -d yolov8 -r -j
# 产物 install/rk3588_linux_aarch64/rknn_yolov8_demo/rknn_yolov8_demo
```

**不用 OpenCV** —— 新版的例子用仓库自带的 `utils/`（`image_utils.c` 等）做图像 I/O。

### 模型

例子自带的 `download_model.sh` 下的是 **ONNX**（不是 .rknn），要用
`rknn-toolkit2` 转：

```bash
wget -O yolov8n.onnx https://ftrg.zbox.filez.com/v2/delivery/data/\
95f00b0fc900458ba134f8b180b3f7a1/examples/yolov8/yolov8n.onnx
python3 convert.py yolov8n.onnx rk3588 i8 yolov8n.rknn   # 见 examples/yolov8/python/
```

`rknn-toolkit2==2.3.2` 必须和板上 `librknnrt` 的版本一致（实测 2.3.2）。
装它有一串版本坑，都踩过了：torch 从 PyPI 下是 797 MB（捆了 CUDA），换
`--index-url https://download.pytorch.org/whl/cpu` 省 4 倍；`scipy` 会把
numpy 顶到 2.x 而 toolkit 要 `<=1.26.4`；**新版 setuptools（≥81）删了
`pkg_resources`**，要钉 `<81`；**onnx ≥1.17 删了 `onnx.mapping`**，要钉 1.16.1。

### ⚠️ 为什么要自带一套 glibc（`/usr/lib/rknn-yolo/`）

两套运行时**需求不同**：

| | 提供到 | demo 要 |
|---|---|---|
| `lib/`（上游运行时，`rknpu2` 用） | `GLIBC_2.30` / `GLIBCXX_3.4.28` | — |
| YOLOv8 demo（ubuntu 交叉链编的） | — | **`GLIBC_2.34` / `GLIBCXX_3.4.29`** |

老的那套**满足不了**新 demo（硬放进去是加载即失败）；把新版塞进
`/usr/lib/rknn/` 又会动到已经真机验证过的 `rknpu2` 那条路。
所以**各带一套、互不干扰** —— `rknpu2` 那条路一个字节都没变。

板上是靠内核那句"未知 syscall 返回 `-ENOSYS`"让 glibc 2.39 的
`clone3`/`rseq`/`futex_waitv` 回退路径生效的。

### ⚠️ 核数：默认是**单核**，要用三核必须显式设置

`librknnrt` 的默认值是 `RKNN_NPU_CORE_AUTO`(0)，注释写的是 "run on NPU core
randomly"，但落到内核里就是**单核** —— vendor 的 `rknpu_schedule_core_index()`
在 AUTO 下强制 `use_core_num = 1`，只挑一个核跑完整个 job。

想用满三个核，应用侧必须调：

```c
rknn_set_core_mask(ctx, RKNN_NPU_CORE_0_1_2);   /* = 7 */
```

**rknn_model_zoo 的 demo 一次都没调它**（静态查过动态符号表，只导入
`rknn_init/query/inputs_set/run/outputs_get/outputs_release/destroy`），
所以原版 demo 跑的是单核。我们打了补丁（`tools/rknn_zoo_core_mask.patch`），
但**它不改变默认行为** —— 只有显式给了参数才去调：

```
~ # yolo            # AUTO（默认，真机验证过）
~ # yolo 1          # 只 core0
~ # yolo 7          # 三核
~ # yolo 7 /tmp/m.rknn /tmp/x.png
```

> ⚠️ **核数走位置参数，不能走环境变量。** 本内核的 `execve` **不传环境变量**
> （`kernel/syscall/core/proc_lifecycle.c` 里 `(void)envp;`），所以
> `RKNN_CORE_MASK=7 yolo` 里的变量根本到不了 demo，`getenv()` 永远是 NULL ——
> 这个坑真机上实际踩过一次（设了 1 却还是跑三核，查了半天以为补丁没生效）。
> `argv` 是可靠的，内核还专门按 CPU 分行拷贝以避免并发覆写。

> ⚠️ **两次尝试之间要重启板子。** 一个作业超时后，对应的核会**卡在半路**
> （`PC_OP_EN` 读回 1、`task_status` 停在中间值），在那之后同一个核上的任何
> 提交都起不来 —— 看起来像"单核也坏了"，其实是上一次的余波。

内核对多核提交会打取证日志（单核是常态，不打，省 UART 时间）：

```
[rknpu] 多核提交 core_mask=0x7 -> 3 核: c0[672+4] c1[923+4] c2[998+4] use_core_num=3
[rknpu]   job=[676,688) subcore_task: [0]=672+4 [1]=923+4 [2]=672+4 [3]=923+4 [4]=998+4
[rknpu] 多核完成: c0[irq=1 done=4/4] c1[irq=1 done=4/4] c2[irq=1 done=4/4]
```

### librknnrt 的切片语义（真机实测确定，别再猜）

`subcore_task[i]` **不落在 job 的 `[task_start, task_start+task_number)` 区间内**。
真机上跑了 25 个作业，规律完全一致：

```
job=[670,676)  n=6   →  c0=670+2  c1=921+2  c2=996+2
job=[676,688)  n=12  →  c0=672+4  c1=923+4  c2=998+4
job=[723,738)  n=15  →  c0=703+5  c1=931+5  c2=1006+5
...
```

两个规律，25/25 条成立：

1. **`n0 + n1 + n2 == job 的 task_number`**（三个核分掉这个作业的活）
2. **三个核各占任务数组里一段互不重叠的连续区域**：

```
c0: 670..921      c1: 921..996      c2: 996..1071     (数组共 1071 条)
```

也就是说 librknnrt 把整个任务缓冲区**按核切成三段**，每个核的活写在自己那段里；
`task_start` 是 c0 那段的游标，`task_number` 是三个核的总数。

> ⚠️ **别自己给切片加校验。** vendor 的 `rknpu_job_subcore_commit_pc()`
> 直接取值、不做任何检查。我一度按"切片必须落在 job 范围内"加了一条校验，
> 真机上把正确的切片判成非法 → 退回单核完整范围 → 12 个任务全塞给 core0 →
> 跑一半卡死，**整颗核卡住**（PC_OP_EN 停在 1），之后同一核上的所有提交都起不来。
> 现在只留缓冲区边界检查（内核自己的安全底线），越界当场拒绝而不是硬凑。

> ⚠️ **两次尝试之间要重启板子。** 作业超时后对应的核会卡住，之后所有提交都失败
> —— 看起来像"别的也坏了"，其实是上一次的余波。

`irq` 是**该核自己那条中断线**（142/143/144）上的次数，`done` 是读**该核自己**
的 `PC_TASK_STATUS` 得到的完成数 —— 两列都是从硬件读的，不是我们写下去的回显。
core1/core2 两列都非零，才说明它们真的在跑。

### 用法

```
~ # yolo                  # 自带模型 + 自带测试图
~ # yolo /usr/lib/rknn-yolo/yolov8n.rknn /tmp/你的图.png
```

检测结果打到 stdout（`名字 @ (x1 y1 x2 y2) 置信度`），带框的图写到
`/tmp/yolo/out.png`。

**图片格式要用 `.png` / `.bmp`** —— 例子把 `.jpg`/`.jpeg` 交给 turbojpeg
（我们 `-j` 把它关了），其它扩展名才走内置的 stb_image。

> demo 里类别名是**相对路径** `./model/coco_80_labels_list.txt`
> （`postprocess.cc:26`），结果图也写当前目录 —— 所以 `/bin/yolo` 会在
> `/tmp/yolo/` 下拼一个工作目录再启动，而不是直接 `cd` 到只读的安装目录。
