# Git 使用笔记

> 针对：Windows + Git Bash / Git for Windows 2.55
> 配套项目：`G:\Boot`（STM32F407 Bootloader）→ https://github.com/rain52-lei/stm32f4_Bootloader
> 更新日期：2026-08-28

---

## 一、核心概念（先建立模型，命令才有意义）

文件在三个"区域"之间流动：

```
工作区              暂存区                本地仓库             远程仓库
(你正在改的)  →  (git add 挑出来的)  →  (git commit 拍快照)  →  (git push 上云)
   改文件          git add -A           git commit -m         git push
```

- **commit = 给整个项目拍一张快照**，永远可以翻回去看
- **push = 把本地快照同步到 GitHub 云端**（备份 + 作品集）
- git 是本地工具，不联网也能用；GitHub 是托管网站，需要账号（已注册：rain52-lei）

---

## 二、本机已经配置好的东西（换电脑/重装系统后需要重做）

| 配置 | 命令（已执行过） |
|---|---|
| 全局身份 | `git config --global user.name "rain52-lei"` |
| 全局邮箱 | `git config --global user.email "2823096107@qq.com"` |
| 中文文件名不乱码 | `git config --global core.quotepath false` |
| 仓库关联远程 | `git remote add origin https://github.com/rain52-lei/stm32f4_Bootloader.git` |
| 首次推送授权 | 弹窗选 "Sign in with your browser"，凭证已存好，以后 push 免密 |
| 推送抗超时参数（本仓库内） | `http.postBuffer` 调大、`http.version HTTP/1.1` 等，见踩坑记录 |

查当前配置：`git config --list --global`（全局）和 `git config --list --local`（本仓库）。

---

## 三、日常四连（90% 的使用就是这一套）

```bash
cd /g/Boot            # 0. 每次开终端先进仓库文件夹
git status            # 1. 看哪些文件动了
git add -A            # 2. 全部改动放进暂存区（或 git add 某个文件）
git commit -m "写清这次改了什么"   # 3. 拍快照
git push              # 4. 同步到 GitHub
```

实战示例——往仓库加一个新文件：

```bash
git status            # 新文件显示红色 "Untracked files"
git add README.md     # 放进暂存区，status 里变绿色
git commit -m "添加项目说明文档"
git push
```

要点：

- **节奏**：每完成一个能说清楚的小目标就 commit 一次（"修复分包头超时错位"），别攒一大坨
- **提交信息**写"为什么改"，以后 `git log --oneline` 靠它检索
- commit 后忘了 push 没关系，快照已在本地；`git status` 会提示
  `Your branch is ahead of 'origin/main' by 1 commit`，想起来再推即可
- 改动多但主题不同的，可以分两次：add 一部分 → commit → add 剩余 → commit

---

## 四、查看历史与改动

```bash
git log --oneline          # 每行一个快照：提交号 + 信息
git log --oneline --stat   # 附带每个提交改了哪些文件
git diff                   # 工作区还没 add 的改动（逐行）
git diff --staged          # 已 add、还没 commit 的改动
git show 提交号            # 看某次快照的具体内容
```

---

## 五、后悔药（按危险程度排序）

```bash
git restore --staged 文件名    # ① 只是"取消暂存"，文件内容不动，安全
git commit --amend --no-edit   # ② 补进上一次提交（忘加文件/改错字时用）
git commit --amend -m "新信息" #    或只改上一次的提交信息
git restore 文件名             # ③ 丢弃未提交的修改！改没了就没了，慎用
git revert 提交号              # ④ 生成一个"反向提交"抵消某次提交，已 push 也能用，安全
git reset --hard 提交号        # ⑤ 时光机回退，之后的提交全部丢弃！危险，初学少碰
```

---

## 六、分支（想大改之前先开一条平行时间线）

```bash
git switch -c 方案B     # 创建并切换到新分支（当前快照的副本）
# ...随便改，随时 commit，不影响主线...
git switch main         # 切回主线
git merge 方案B         # 把分支上的成果合并回主线
git branch -d 方案B     # 合并完删除分支
git branch -a           # 看所有分支
```

适用场景：A/B 双槽想试两种实现、大重构前留后路。
（不要用"复制整个文件夹"来开方案——那就是分支存在的意义。）

---

## 七、标签（给验证通过的固件版本立路标）

```bash
git tag -a v3-flash_ok -m "首次完整跑通升级流程"   # 在当前提交上打标签
git push origin v3-flash_ok                        # 标签也要单独推送
git tag                                            # 列出所有标签
git checkout v3-flash_ok                           # 翻回那个版本看代码
```

---

## 八、远程仓库（GitHub）相关

```bash
git remote -v                       # 查看本仓库关联的远程地址
git push                            # 推送（首次用 git push -u origin main 建立关联）
git pull                            # 拉取云端更新（多台电脑/多人协作时用）
git clone https://github.com/rain52-lei/xxx.git   # 把云端仓库整个下载到新电脑
```

换电脑时的标准流程：装 Git → 配身份（第二节）→ `git clone` 仓库地址 → 继续干活。

---

## 九、新建项目时的 Git 初始化（背下来）

```bash
cd 新项目文件夹
git init -b main          # 文件夹变仓库
# 写 .gitignore（Keil 项目直接把 G:\Boot\.gitignore 抄去，规则通用）
git add -A
git commit -m "初始提交"
# 想上 GitHub 的话：网页建一个【空】仓库（三个勾选框都不勾），然后：
git remote add origin https://github.com/rain52-lei/仓库名.git
git push -u origin main
```

记住：**身份等全局配置全机器只配一次；每个新项目 init 一次；之后永远是日常四连。**

什么该进仓库：手写出来的东西（源码、.ioc、.sct、笔记、脚本）。
什么不进：能重新生成的（编译产物 .o/.axf/.map/.bin）——写进 `.gitignore`。

---

## 十、踩坑记录（都是实际踩过的）

### 1. local 配置覆盖 global —— "改了配置没生效"
`git config` 有三层：system（整机）< global（用户）< **local（单个仓库，优先级最高）**。
本次在 `G:\Boot` 里先配了 local 占位身份，后改 global 被 local 压住，作者没变。
**排查口诀**：配置改了没反应，先 `git config --list --local` 看仓库里有没有覆盖项。

### 2. push 报 HTTP 408 / RPC failed —— 国内直连 GitHub 传输超时
表现：`error: RPC failed; HTTP 408 curl 22`、`the remote end hung up unexpectedly`。
本质：大包上传到 GitHub 的链路波动被掐断，和操作无关。
**对策**：直接重试一两次；本仓库已加抗超时参数（postBuffer 调大 + HTTP/1.1）。
新仓库如果也常超时，把同样四条 config 在新仓库里跑一遍：

```bash
git config http.postBuffer 524288000
git config http.lowSpeedLimit 0
git config http.lowSpeedTime 999999
git config http.version HTTP/1.1
```

还不行就换 Gitee（命令一字不差，只换远程网址），或以后学 SSH 方式（443 端口）。

### 3. 提交时刷屏 "LF will be replaced by CRLF"
Windows 和 Linux 换行符差异的提醒，**无害，可无视**。

### 4. git status 中文文件名显示成 \345\255\244... 乱码
`git config --global core.quotepath false` 一次解决（已配）。

### 5. 改了历史（amend）之后提交号变了
`git commit --amend` 会重写最后一次提交，提交号必然变化（7b7f80b → 1453016），这是正常的。
**注意**：已经 push 的提交不要随便 amend，会产生历史分叉——想改已推送的内容用 `revert`。

---

## 十一、命令速查表

| 场景 | 命令 |
|---|---|
| 看状态 | `git status` |
| 看改动 | `git diff` / `git diff --staged` |
| 暂存 | `git add -A` 或 `git add 文件` |
| 提交 | `git commit -m "信息"` |
| 推送 | `git push` |
| 拉取 | `git pull` |
| 历史 | `git log --oneline [--stat]` |
| 取消暂存 | `git restore --staged 文件` |
| 丢弃未提交修改 | `git restore 文件`（危险） |
| 修补上次提交 | `git commit --amend` |
| 分支 | `git switch -c 名` / `git switch main` / `git merge 名` |
| 标签 | `git tag -a v1 -m "..."` / `git push origin v1` |
| 新仓库 | `git init -b main` |
| 关联远程 | `git remote add origin URL` |
| 配置 | `git config --global user.name "..."` 等 |

---

## 十二、接下来的学习路线

1. ✅ 已会：init / add / commit / push / 查历史 / 远程关联
2. 下一周：restore、amend、分支（第六节），在自己的项目里真刀真枪练
3. 下一月：标签管理固件版本、`git diff` 熟练阅读、.gitignore 进阶
4. 之后：GitHub 上发 Release（挂固件 bin 附件）、Pull Request 流程（给别人项目提代码）

---

## 十三、可视化工具（不想敲命令时用）

**零安装，现在就能用**——Git for Windows 自带图形历史查看器：

```bash
gitk                                # 弹窗图形化提交历史 + 每次提交的 diff
git log --graph --oneline --all     # 终端里的 ASCII 分支图
```

**本机已装 VS Code**（D 盘），这是日常主力方案：

- 左侧"源代码管理"面板：改动的文件列表 = `git status`，点 + 暂存 = `git add`，填信息点 ✓ = `git commit`，同步按钮 = push/pull
- 点击任意文件可看逐行 diff；行号旁点一下还能单行暂存
- 装一个 **Git Graph** 插件（扩展商店搜 Git Graph）：分支拓扑图、右键任意提交可 checkout/cherry-pick/revert

其他常见 GUI（按需再装，本机目前都没有）：

| 工具 | 特点 | 适合 |
|---|---|---|
| GitHub Desktop | GitHub 官方、极简，按钮 = add/commit/push | 只想要最简单的 |
| TortoiseGit | 资源管理器右键直接操作 | 习惯右键流的人 |
| Sourcetree | 独立软件，分支图直观、功能全 | 想认真玩分支/多仓库 |

**提醒**：GUI 只是命令的按钮化（暂存 = staged、✓ = commit、同步 = push），底层概念还是本笔记这一套。但 amend、revert、解冲突这类复杂操作 GUI 反而容易点错——命令行永远是兜底技能，速查表（第十一节）别扔。
