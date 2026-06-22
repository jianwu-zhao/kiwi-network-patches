# Kiwi Browser GitHub Actions 云编译指南

## 前置条件

- 一个 GitHub 账号（免费）
- Kiwi 网络补丁包（已包含在本项目中）

---

## 第一步：在 GitHub 上创建仓库

```bash
# 1. 登录 https://github.com
# 2. 点击右上角 "+" → "New repository"
# 3. 仓库名: kiwi-network-patches
# 4. 设为 Private 或 Public（随意）
# 5. 创建
```

## 第二步：上传补丁代码

```bash
# 本地（有 git 的机器）执行

# 克隆刚创建的仓库
git clone https://github.com/<你的用户名>/kiwi-network-patches.git
cd kiwi-network-patches

# 解压补丁包
tar xzf /path/to/kiwi-network-patches.tar.gz

# 移动文件到根目录
mv kiwi-network-patches/* .
rmdir kiwi-network-patches

# 推送到 GitHub
git add .
git commit -m "Kiwi network patches: ECH + Kyber + DoH + QUIC/HTTP3"
git push
```

## 第三步：添加 Actions 工作流

```bash
# 在 GitHub 网页端操作:
# 1. 打开你的仓库 → Actions 标签
# 2. 点击 "set up a workflow yourself"
# 3. 将以下内容粘贴进去
```

复制 `github-actions/build-kiwi.yml` 的内容到编辑器中，然后点 "Start commit"。

## 第四步：触发构建

方式一：手动触发
1. 打开仓库 → Actions 标签
2. 选择 "Build Kiwi Browser APK (arm64)"
3. 点击 "Run workflow" → "Run workflow"

方式二：自动触发（已配置好）
- 推送代码到 `kiwi` 分支时自动构建

## 第五步：下载 APK

1. 构建完成后（约 2-4 小时）
2. 进入 Actions 页面 → 点击成功的 workflow run
3. 在 "Artifacts" 区域下载 `kiwi-browser-arm64.zip`
4. 解压得到 `.apk` 文件

---

## 注意事项

### ⚠️ 资源限制
GitHub 免费 runner (ubuntu-latest):
| 资源 | 配额 | 实际需求 | 结果 |
|------|------|---------|------|
| RAM  | 7 GB | 16+ GB  | ⚠️ 可能 OOM |
| CPU  | 4核  | 8核推荐 | ⚠️ 较慢 |
| 磁盘 | 14 GB | 50+ GB  | ❌ 不足 |
| 超时 | 6小时| 2-4小时 | ✅ 够用 |

**免费 runner 磁盘空间严重不足**。建议升级方案：

### ✅ 推荐：使用更大的 runner（付费）

编辑 `.github/workflows/build-kiwi.yml`，将：

```yaml
runs-on: ubuntu-latest
```

改为：

```yaml
runs-on: ubuntu-24-core-128gb-ssd
```

GitHub 计价：$0.48/分钟（24核机型），完整构建约 $60-120。

### ✅ 省钱技巧

1. **使用 ccache** — 第二次构建只需 30 分钟
2. **分步构建** — 先验证 `kiwi_net_extensions`，再编译完整 APK
3. **本地交叉编译** — 用自己电脑做最后的 APK 打包

### ✅ 更经济的方案：腾讯云/阿里云

```bash
# 腾讯云 SA2.2XLARGE64 (8核 16GB)
# 约 ¥60/天，构建一次绰绰有余

# 阿里云 ecs.g6e.2xlarge (8核 16GB)
# 约 ¥80/天
```

---

## 验证构建是否成功

构建过程中，GitHub Actions 日志会显示：

```
=== 编译 kiwi_net_extensions ===
[1/12] CXX obj/net/kiwi/ssl_client_socket.o
[2/12] CXX obj/net/kiwi/host_resolver_manager.o
...
[12/12] LINK ./kiwi_net_extensions
✓ kiwi_net_extensions 编译成功

=== 编译 chrome_public_apk ===
[1/50000] CXX ...
...
[50000/50000] STAMP chrome_public_apk.apk
✓ APK 生成成功!
```

看到最后一行 `STAMP chrome_public_apk.apk` 即表示构建成功。
