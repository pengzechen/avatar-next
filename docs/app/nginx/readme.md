# nginx on Avatar OS

本文档记录当前可运行的 AArch64 nginx 用户态程序构建方式。

当前二进制：`apps/nginx-aarch64`

当前版本：nginx `1.26.3`

当前状态：可通过 lwIP/virtio-net 提供 HTTP 服务；当前这版主要用于验证 TCP/HTTP 通路，尚未确认 nginx configure 是否真正启用 epoll。

## 下载源码

```bash
mkdir -p /tmp/opencode
cd /tmp/opencode
curl -LO https://nginx.org/download/nginx-1.26.3.tar.gz
tar xf nginx-1.26.3.tar.gz
cd nginx-1.26.3
```

## 交叉编译前补丁

nginx 的 configure 会尝试运行测试程序。AArch64 交叉编译时这些测试程序不能在 host 上直接运行，所以需要禁用 feature run，并手动给出当前 Avatar/musl AArch64 环境需要的少量结果。

修改 `auto/cc/name`，让 configure 不运行探测程序：

```diff
-ngx_feature_run=yes
+ngx_feature_run=no
```

修改 `auto/types/sizeof`，为 AArch64 LP64 ABI 硬编码基础类型大小。当前使用的值：

```text
sizeof(int) = 4
sizeof(long) = 8
sizeof(long long) = 8
sizeof(void *) = 8
sizeof(size_t) = 8
sizeof(off_t) = 8
sizeof(time_t) = 8
```

如果 configure 后 `objs/ngx_auto_config.h` 中没有这些定义，手动补上：

```c
#ifndef NGX_HAVE_LITTLE_ENDIAN
#define NGX_HAVE_LITTLE_ENDIAN 1
#endif

#ifndef NGX_HAVE_MAP_ANON
#define NGX_HAVE_MAP_ANON 1
#endif
```

## configure 参数

当前可运行版本使用静态 musl AArch64 工具链，并关闭 PCRE、zlib、OpenSSL 以及依赖它们的模块：

```bash
CC=aarch64-linux-musl-gcc ./configure \
  --prefix=/etc/nginx \
  --sbin-path=/bin/nginx \
  --conf-path=/etc/nginx/nginx.conf \
  --error-log-path=/tmp/nginx-error.log \
  --pid-path=/tmp/nginx.pid \
  --lock-path=/tmp/nginx.lock \
  --http-log-path=/tmp/nginx-access.log \
  --with-cc=aarch64-linux-musl-gcc \
  --with-cc-opt='-static -fPIE' \
  --with-ld-opt='-static -pie' \
  --without-pcre \
  --without-http_rewrite_module \
  --without-http_gzip_module \
  --without-http_ssi_module \
  --without-http_userid_module \
  --without-http_auth_basic_module \
  --without-http_autoindex_module \
  --without-http_geo_module \
  --without-http_map_module \
  --without-http_split_clients_module \
  --without-http_referer_module \
  --without-http_proxy_module \
  --without-http_fastcgi_module \
  --without-http_uwsgi_module \
  --without-http_scgi_module \
  --without-http_memcached_module \
  --without-http_limit_conn_module \
  --without-http_limit_req_module \
  --without-http_empty_gif_module \
  --without-http_browser_module \
  --without-http_upstream_hash_module \
  --without-http_upstream_ip_hash_module \
  --without-http_upstream_least_conn_module \
  --without-http_upstream_random_module \
  --without-http_upstream_keepalive_module \
  --without-http_upstream_zone_module \
  --without-mail_pop3_module \
  --without-mail_imap_module \
  --without-mail_smtp_module
```

编译并复制到项目：

```bash
make -j$(nproc)
cp objs/nginx /home/ajax/Desktop/Project/Kernel/avatar-next/apps/nginx-aarch64
```

## rootfs 集成

项目 `Makefile` 会在构建 rootfs 时安装：

```text
apps/nginx-aarch64 -> /bin/nginx
/etc/nginx/nginx.conf
/www/index.html
```

重建内核和 rootfs：

```bash
make ARCH=aarch64 ETH=virtio NGINX_TEST=1 kernel
make ARCH=aarch64 rootfs
```

`NGINX_TEST=1` 会禁用内核内建 HTTP server，避免它占用 `:80`。启动日志应包含：

```text
[http] kernel HTTP server disabled for nginx test
```

## 网络测试

当前内核 lwIP 静态网络配置：

```text
guest IP: 192.168.100.2
gateway:  192.168.100.1
```

建议使用 TAP，而不是 QEMU user net：

```bash
sudo ip tuntap add dev tap0 mode tap user "$USER"
sudo ip addr add 192.168.100.1/24 dev tap0
sudo ip link set tap0 up
```

启动 QEMU：

```bash
make ARCH=aarch64 ETH=virtio run-net LOG=info NGINX_TEST=1 \
  QEMU_NET_FLAGS='-netdev tap,id=net0,ifname=tap0,script=no,downscript=no -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56' \
  -j8
```

QEMU shell 中启动 nginx：

```sh
/bin/nginx -g 'master_process off;'
```

Host 上访问：

```bash
curl http://192.168.100.2/
```

预期输出：

```html
<html><body><h1>Avatar nginx</h1></body></html>
```

## 已知限制

当前二进制是手工外部构建产物，还没有纳入项目内可复现构建流程。

当前这版 nginx 能提供 HTTP 服务，但 configure 阶段的事件模块探测还需要继续整理。下一步计划构建并验证明确使用 epoll 的 nginx。
