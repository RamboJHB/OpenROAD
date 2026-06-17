# Prompt Templates

根据日常在 OpenROAD 上的高频工作流整理的提示词模版。直接复制、替换 `<尖括号>`
占位符即可使用。每条模版都已内置项目的关键约束（clang-format、`git commit -s`、
CMake + Bazel 双注册、`src/sta/` 上游管理等），以便一次说清需求、减少来回。

> 约定：`<MODULE>` 取模块前缀（ant / cts / dpl / drt / gpl / grt / mpl / odb /
> pad / pdn / psm / rcx / rsz / sta / stt / tap / upf / utl …）；`<ISSUE#>` 为
> GitHub issue 号；`<CODE>` 为错误码（如 `GRT-0001`、`GPL-0305`）。

---

## 1. 修 bug（issue 号 / 错误码）

```
修复 <MODULE> 的 bug：<ISSUE# 或 CODE>。
请遵循 "trace bugs upstream"：定位坏数据的产生处而非报错处。
完成后补一个能复现该失败模式的回归测试（同样的错误码 / 同样的触发条件），
并在 CMake 和 Bazel 同时注册；对改动的 C++ 跑 clang-format（不要碰 src/sta/* 和 *.i）；
用 git commit -s 提交，commit message 带 "Fixes #<ISSUE#>"。
```

> 也可直接走 skill：`/fix-bug <ISSUE# 或 CODE>`

---

## 2. 加测试（务必双注册）

```
给 <MODULE> 加一个测试：<要覆盖的行为/场景>。
写好 Tcl/C++ 测试与 golden 文件，并在 CMake 和 Bazel **都**注册
（最常见的疏漏就是漏了 Bazel）。本地跑通后给我结果。
```

> 也可直接走 skill：`/add-test <MODULE> <描述>`

---

## 3. 修 clang-format 违规

```
修复 <MODULE>（或 <文件路径>）的 clang-format 违规。
只跑 clang-format -i 于受影响的 C++ 文件，绝不格式化 src/sta/* 和 *.i 文件。
改完用 git commit -s 提交，message 形如 "<MODULE>: fix clang-format violations in <file>"。
```

---

## 4. Review PR

```
帮我 review PR <PR# 或 URL>。
按项目优先级输出 review 笔记：correctness > QoR impact > testing > architecture > style > process。
只生成草稿笔记给我看，不要直接发到 GitHub。
注意：URL 可能来自 fork，REPO 要从 URL 路径解析，别默认 The-OpenROAD-Project/OpenROAD。
```

> 也可直接走 skill：`/review-pr <PR# 或 URL>`

---

## 5. 跟 CI / 让 CI 转绿

```
PR <PR#> 的 CI 失败了，帮我看下原因并修。
注意：
- 不要靠 close/reopen 重触发 CI（需要 admin 权限）；要重跑就用
  git commit --allow-empty -s -m "retrigger CI"。
- gh pr checks 退出码 8 只是 pending，不是错误。
- Clang-Tidy CI 可能因为未解决的 review 评论而挂，不一定是代码问题。
本地复现可用：bazel build --config=lint //src/<MODULE>/...
```

---

## 6. Triage issue（带复现物的 issue）

```
triage issue <ISSUE#>：用附件 tarball 复现 bug，并用 whittle.py 最小化测试用例。
给我最小复现脚本、定位到的模块/函数，以及下一步建议。
```

> 也可直接走 skill：`/triage-issue <ISSUE#>`

---

## 7. 更新 STA / SDC（上游谨慎）

```
我需要处理与 STA/SDC 相关的问题：<描述>。
重要约束：src/sta/ 由上游（OpenSTA）管理，改动前先问我；
优先在 OpenROAD 侧解决（如 src/dbSta/、src/rsz/）。
若根因确实在 OpenSTA，请报告结论而不要直接打补丁。
```

---

## 8. 重新生成产物 / 文档（如 railroad diagram、SVG）

```
重新生成 <模块/产物，例如 odb 的 LEF/DEF railroad diagram SVG>。
说明用到的生成脚本/命令，确认输出无意外 diff，再用 git commit -s 提交，
message 形如 "<MODULE>: regenerate <artifact>"。
```

---

## 9. 实现新功能 / 重构模块

```
在 <MODULE> 中实现/重构：<需求描述>。
遵循 docs/agents/coding.md 的编码规范（如面积计算用 int64_t、error()/throw 之后删除不可达代码）。
新增/改动行为请配套测试并在 CMake + Bazel 双注册；
C++ 跑 clang-format（除 src/sta/* 与 *.i）；用 git commit -s 提交。
```

---

## 10. 提交前自检清单

```
在提交前帮我核对：
- [ ] 测试通过：cd src/<MODULE>/test && ./regression -R <TEST_NAME>
- [ ] git diff HEAD 无意外改动
- [ ] 测试已在 CMake 和 Bazel 双注册
- [ ] 未修改任何 src/sta/ 文件
- [ ] C++ 已跑 clang-format（未碰 *.i）
- [ ] commit 已签名（git commit -s）
确认无误后推送到当前 feature 分支（git push -u origin <branch>），不要建 PR 除非我明说。
```

---

## 使用提示

- 一次性把模块、约束、期望产物说清，能显著减少来回确认。
- 不确定模块前缀时，可直接贴报错码或文件路径，让我自行推断模块。
- 需要的话可把以上任意一条沉淀为新的 skill（放 `.agents/skills/<name>/SKILL.md`）。
