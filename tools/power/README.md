# 米家智能插座控制工具

命令行控制米家 Gra 智能控制器 V7 插座的开关。

## 前置要求

```bash
pip install python-miio
```

## 配置

编辑 `miplug` 脚本中的设备信息：

```python
DEVICE_IP = "192.168.1.52"           # 设备 IP 地址
DEVICE_TOKEN = "9e133ad5c0bbcaa..."  # 设备 Token（32位）
```

### 获取 Token 方法

1. **Token 提取工具**（推荐）
   ```bash
   pip install python-miio-cloud-tokens
   miiocli cloud login
   miiocli cloud devices
   ```

2. **Android 抓包**
   - 安装 Packet Capture
   - 抓包米家 App 的请求
   - 找到 URL 中的 `token` 参数

3. **米家日志**（Android）
   - 米家 App → 设置 → 开启调试日志
   - 导出日志，搜索 `token`

## 使用方法

```bash
# 直接使用脚本
./tools/power/miplug on      # 开启插座
./tools/power/miplug off     # 关闭插座
./tools/power/miplug         # 切换状态（默认）
```

### 设置别名（可选）

在 `~/.bashrc` 中添加：

```bash
alias miplug="/home/ajax/Proj/OS/Thread-Process-Lock/avatar/tools/power/miplug"
```

之后可在任意位置使用：

```bash
miplug on
miplug off
miplug
```

## 命令参数

| 参数 | 功能 |
|------|------|
| `on`, `1`, `true` | 开启插座 |
| `off`, `0`, `false` | 关闭插座 |
| 其他或无参数 | 切换开关状态 |

## 故障排查

```bash
# 检查设备连接
python3 -m miio.cli device --ip <IP> --token <TOKEN> info

# 查看设备属性
python3 -m miio.cli genericmiot --ip <IP> --token <TOKEN> properties
```
