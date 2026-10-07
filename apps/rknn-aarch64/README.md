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
