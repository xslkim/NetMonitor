# 免密 sudo 配置指南

> 本文档说明如何为视频制作 Agent 配置免密 sudo，使 Stage 0 的系统依赖安装可以全自动执行。

---

## 为什么需要免密 sudo

视频制作工作流（`VIDEO_AGENT_WORKFLOW.md` Stage 0）需要通过 `apt` 安装多个系统包（ffmpeg、Chromium、中文字体、Node.js 等）。这些命令需要 `sudo` 权限。

如果 Agent 运行时 `sudo` 要求输入密码，自动化流程会卡住。配置免密 sudo 后，Agent 可以无人值守地完成全部安装。

---

## 方案一：为当前用户配置完全免密 sudo（简单但权限较大）

适用场景：专用的视频制作服务器 / 虚拟机 / 容器，只有你一个用户。

```bash
# 1. 用当前有 sudo 权限的用户执行（会要求输入一次密码）
sudo visudo -f /etc/sudoers.d/video-agent

# 2. 在打开的编辑器中写入以下内容（将 username 替换为你的实际用户名）：
username ALL=(ALL) NOPASSWD: ALL

# 3. 保存退出。visudo 会自动检查语法，如果有错会提示你修正。
```

验证：

```bash
# 退出当前 shell 重新登录，然后测试：
sudo -n whoami
# 应输出 root，不要求密码
```

---

## 方案二：仅对特定命令免密（更安全，推荐生产环境）

适用场景：多人共用的服务器，或者你只想开放最小必要权限。

```bash
sudo visudo -f /etc/sudoers.d/video-agent
```

写入以下内容：

```sudoers
# 只允许 apt、snap、dpkg、fc-cache 免密执行
username ALL=(ALL) NOPASSWD: /usr/bin/apt, /usr/bin/apt-get, /usr/bin/snap, /usr/bin/dpkg, /usr/bin/fc-cache
```

> **注意**：如果工作流中还需要其他 sudo 命令（如 `bash -c` 安装 NodeSource），需要额外添加对应路径。

---

## 方案三：使用预装镜像（最安全，无需免密 sudo）

如果你不想配置免密 sudo，可以提前手动安装所有依赖，然后让 Agent 跳过 Stage 0 的安装步骤。

手动预装命令：

```bash
# 一次性执行，需要输入密码
sudo apt update && sudo apt upgrade -y
sudo apt install -y curl wget git build-essential ffmpeg chromium-browser fonts-noto-cjk fonts-noto-cjk-extra python3 python3-pip python3-venv

# 安装 Node.js 20
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs

# 安装 edge-tts
python3 -m venv ~/video-agent-venv
source ~/video-agent-venv/bin/activate
pip install edge-tts
```

安装完成后，Agent 的 Stage 0 只需执行验证步骤（0.5 节），跳过安装命令。

---

## 安全注意事项

| 方案 | 安全级别 | 适用场景 |
|------|---------|---------|
| 方案一（完全免密） | 低 | 一次性虚拟机、Docker 容器、个人开发机 |
| 方案二（部分免密） | 中 | 共享服务器、长期运行环境 |
| 方案三（预装镜像） | 高 | 生产环境、不允许修改 sudoers 的环境 |

**通用安全建议**：
- 免密 sudo 配置完成并且视频渲染成功后，建议**删除**免密配置：`sudo rm /etc/sudoers.d/video-agent`
- 不要将 sudo 密码写入环境变量、脚本文件或配置文件
- 如果使用 Docker，直接以 root 用户运行容器即可，不需要 sudo

---

## 验证免密 sudo 是否生效

```bash
# 方法 1：-n 标志表示非交互模式，如果需要密码会直接失败
sudo -n true && echo "免密 sudo 已生效" || echo "免密 sudo 未生效"

# 方法 2：测试实际命令
sudo -n apt --version
```

Agent 在 Stage 0 开始时会自动执行 `sudo -n true` 检测，如果免密 sudo 不可用会给出提示。
