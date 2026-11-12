---
name: tool-npu-rep-html
description: >-
  把 Ascend/CANN `.npu-rep`（或经典 cann-rep）打成一份可离线打开的交互式 HTML 报告。
  未缓存时从 GitCode opdevtools/plugin_release 最新 release 拉取零依赖 CLI `npu-rep-html.mjs`，
  并在 Node 缺失或主版本低于 20 时安装 Node 20。适用于 Linux、macOS、Windows。
  触发：用户要 HTML 报告、离线报告、file:// 分享、CI profiling HTML 产物、npu-rep-html，
  或要把 `.npu-rep` 转成浏览器可打开的 HTML。
license: CANN-2.0
---

# npu-rep HTML 报告

把 `.npu-rep` 转成一份可离线打开的交互式 HTML（泳道查看器）。需要 **Node ≥ 20**，以及来自最新
[opdevtools/plugin_release](https://gitcode.com/opdevtools/plugin_release) GitCode release 的
`npu-rep-html.mjs`（当前标签示例为 `1.5.5`）。不要把下载到的脚本发布或提交进仓库。

在 Windows 上（含 Git Bash，`uname -s` 为 `MINGW*`、`MSYS*` 或 `CYGWIN*`）使用 PowerShell 代码块和
`%LOCALAPPDATA%`，不要在该环境安装 Linux tarball。在 PowerShell 中调用 `curl.exe`，不要调用 `curl`
（`curl` 是 `Invoke-WebRequest` 的别名）。

## 解析脚本路径

1. 若设置了 `NPU_REP_HTML`，使用该路径且不要下载。若路径缺失或不是文件，停止并报告该路径。
2. 若未设置 `NPU_REP_HTML`，解析最新 release（无需 token）：

```bash
curl -fsSL "https://api.gitcode.com/api/v5/repos/opdevtools/plugin_release/releases/latest"
```

Windows 上用 `curl.exe -fsSL` 与同一 URL。读取 `tag_name`。在 `assets` 中取 `name` 为
`npu-rep-html.mjs` 的条目，使用其 `browser_download_url`
（`https://gitcode.com/opdevtools/plugin_release/releases/download/<tag>/npu-rep-html.mjs`）。

3. 缓存路径：
   - Linux 与 macOS：`$HOME/.cache/npu-rep-html/<tag>/npu-rep-html.mjs`
   - Windows：`%LOCALAPPDATA%\npu-rep-html\<tag>\npu-rep-html.mjs`
4. **仅当**该缓存路径存在、非空、且首行为 `#!/usr/bin/env node` 时跳过下载。临时文件或部分下载不算缓存命中。
5. 否则用**跟随重定向的 GET**下载（`curl -fsSL` / `curl.exe -fsSL` 已跟随）。**不要**用
   `curl -I` / HEAD：会返回 **401**。GET 会 **302** 到 `file-cdn.gitcode.com`，再得到脚本。
   先把响应体写到标签目录下的临时文件；仅当 curl 退出码为 0、文件非空、且首行为
   `#!/usr/bin/env node` 时再移到缓存路径。任一检查失败则删除临时文件并停止，不要在缓存路径留下残缺文件。

Linux 与 macOS：

```bash
mkdir -p "$HOME/.cache/npu-rep-html/<tag>"
cache="$HOME/.cache/npu-rep-html/<tag>/npu-rep-html.mjs"
tmp=$(mktemp "$HOME/.cache/npu-rep-html/<tag>/npu-rep-html.mjs.XXXXXX")
if curl -fsSL -o "$tmp" "<browser_download_url>" \
  && [ -s "$tmp" ] \
  && [ "$(head -n 1 "$tmp")" = '#!/usr/bin/env node' ]; then
  mv "$tmp" "$cache"
else
  rm -f "$tmp"
  echo "npu-rep-html download failed" >&2
  exit 1
fi
```

Windows（PowerShell）：

```powershell
$ErrorActionPreference = 'Stop'
$dir = Join-Path $env:LOCALAPPDATA "npu-rep-html\<tag>"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$cache = Join-Path $dir "npu-rep-html.mjs"
$tmp = Join-Path $dir ("npu-rep-html.mjs." + [guid]::NewGuid().ToString("N"))
& curl.exe -fsSL -o "$tmp" "<browser_download_url>"
$ok = $LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $tmp) -and ((Get-Item -LiteralPath $tmp).Length -gt 0) -and ((Get-Content -LiteralPath $tmp -TotalCount 1) -eq '#!/usr/bin/env node')
if (-not $ok) {
  Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
  throw "npu-rep-html download failed"
}
# Same-volume replace. File.Replace is atomic; do not delete $cache first.
if (Test-Path -LiteralPath $cache) {
  [System.IO.File]::Replace($tmp, $cache, [NullString]::Value)
} else {
  [System.IO.File]::Move($tmp, $cache)
}
```

不要在工作目录、`~/bin` 或 `.npu-rep` 旁搜索脚本。不要运行 `npm run build:report-shell`。

本技能只做离线泳道 HTML 分享/CI 产物。性能诊断报告（瓶颈判定、`report.md`/`report.html`）走本仓 `tool-perf-analysis`，不要混用。

## Node

运行 `node -p "process.versions.node"`。若主版本 **≥ 20**，使用该 `node`，不要降级到更旧版本。

若缺少 `node` 或主版本 **< 20**，安装当前 **Node 20** 线，将其前置到 `PATH` 后再检查。安装失败或主版本仍 < 20 时停止。

优先使用机器上已有的版本管理器。Linux 与 macOS：`nvm install 20 && nvm use 20`，或
`fnm install 20 && fnm use 20`，或 `n 20`。Windows：`fnm install 20`，然后
`fnm env --use-on-cd --shell powershell | Out-String | Invoke-Expression`，再 `fnm use 20`；
或 nvm-windows `nvm install 20` 并 `nvm use` 该 20.x。`n` 不在 Windows 上运行。若
nvm-windows `nvm use` 因 shell 未提权失败，改用下方 zip 安装。

否则下载官方 Node 20 归档。提取前要求
`https://nodejs.org/dist/latest-v20.x/SHASUMS256.txt` 中的 SHA-256 匹配。把校验和文件、归档和目录树先放到 live 前缀外的暂存区；仅当暂存树含 `node` 二进制后再替换 live 前缀。替换时先把旧 live 挪到旁路备份，再 `mv`/`Move-Item` 暂存树就位；成功后删备份。任一步失败则删除暂存并尽量恢复备份，不要在失败路径上留下空的 live 前缀。

Linux 与 macOS：安装到 `$HOME/.local/npu-rep-node`（二进制 `bin/node`）。若该前缀已有主版本 ≥ 20 的
`bin/node`，仅前置 `$HOME/.local/npu-rep-node/bin`。按 `uname -s` / `uname -m` 匹配
（`Linux`→`linux`，`Darwin`→`darwin`，`x86_64`→`x64`，`aarch64` 或 `arm64`→`arm64`）到
`node-v20.*-<os>-<arch>.tar.xz`。`MINGW*`、`MSYS*`、`CYGWIN*` 视为 Windows——停止并改用 PowerShell 代码块。

```bash
mkdir -p "$HOME/.local"
sums=$(mktemp)
tarball=$(mktemp)
stage=$(mktemp -d "$HOME/.local/npu-rep-node.staging.XXXXXX")
backup="$HOME/.local/npu-rep-node.bak.$$"
case "$(uname -s)" in
  Linux) os=linux ;;
  Darwin) os=darwin ;;
  MINGW*|MSYS*|CYGWIN*) echo "Windows detected ($(uname -s)); use the PowerShell Node block" >&2; exit 1 ;;
  *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac
case "$(uname -m)" in x86_64) arch=x64 ;; aarch64|arm64) arch=arm64 ;; *) echo "unsupported arch: $(uname -m)" >&2; exit 1 ;; esac
curl -fsSL -o "$sums" "https://nodejs.org/dist/latest-v20.x/SHASUMS256.txt" \
  && file=$(awk -v os="$os" -v arch="$arch" '$2 ~ "^node-v20\\.[0-9.]+-" os "-" arch "\\.tar\\.xz$" { print $2; exit }' "$sums") \
  && [ -n "$file" ] \
  && curl -fsSL -o "$tarball" "https://nodejs.org/dist/latest-v20.x/${file}" \
  && expected=$(awk -v file="$file" '$2 == file { print $1; exit }' "$sums") \
  && if command -v sha256sum >/dev/null 2>&1; then actual=$(sha256sum "$tarball" | awk '{ print $1 }'); else actual=$(shasum -a 256 "$tarball" | awk '{ print $1 }'); fi \
  && [ -n "$expected" ] && [ "$actual" = "$expected" ] \
  && tar -xJf "$tarball" -C "$stage" --strip-components=1 \
  && test -x "$stage/bin/node" \
  && { [ ! -e "$HOME/.local/npu-rep-node" ] || mv "$HOME/.local/npu-rep-node" "$backup"; } \
  && mv "$stage" "$HOME/.local/npu-rep-node" \
  && rm -rf "$backup"
status=$?
rm -f "$sums" "$tarball"
if [ "$status" -ne 0 ]; then
  if [ ! -x "$HOME/.local/npu-rep-node/bin/node" ] && [ -d "$backup" ]; then
    rm -rf "$HOME/.local/npu-rep-node"
    mv "$backup" "$HOME/.local/npu-rep-node"
  fi
  if [ ! -x "$stage/bin/node" ]; then rm -rf "$stage"; fi
  exit "$status"
fi
export PATH="$HOME/.local/npu-rep-node/bin:$PATH"
```

Windows：安装到 `%LOCALAPPDATA%\npu-rep-node`（zip 根目录即有 `node.exe`，不在 `bin` 下）。若该
`node.exe` 已是主版本 ≥ 20，仅前置其目录。架构：优先 `PROCESSOR_ARCHITEW6432`，否则
`PROCESSOR_ARCHITECTURE`。`AMD64`→`win-x64`，`ARM64`→`win-arm64`。其他架构停止（没有 Node 20
`win-x86` 构建）。

```powershell
$ErrorActionPreference = 'Stop'
$live = Join-Path $env:LOCALAPPDATA "npu-rep-node"
$liveNode = Join-Path $live "node.exe"
if ((Test-Path -LiteralPath $liveNode) -and ([int](& $liveNode -p "process.versions.node").Split('.')[0] -ge 20)) {
  $env:PATH = "$live;$env:PATH"
} else {
  $cpu = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
  $plat = switch ($cpu) { 'AMD64' { 'win-x64' } 'ARM64' { 'win-arm64' } default { throw "unsupported arch: $cpu" } }
  $work = Join-Path $env:LOCALAPPDATA ("npu-rep-node.staging." + [guid]::NewGuid().ToString("N"))
  $backup = Join-Path $env:LOCALAPPDATA ("npu-rep-node.bak." + [guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Force -Path $work | Out-Null
  $sums = Join-Path $work "SHASUMS256.txt"
  $zip = Join-Path $work "node.zip"
  $extract = Join-Path $work "extract"
  try {
    & curl.exe -fsSL -o "$sums" "https://nodejs.org/dist/latest-v20.x/SHASUMS256.txt"
    if ($LASTEXITCODE -ne 0) { throw "checksum download failed" }
    $hit = Select-String -LiteralPath $sums -Pattern ('^([0-9a-f]{64})\s+(node-v20\.[0-9.]+-' + [regex]::Escape($plat) + '\.zip)$') | Select-Object -First 1
    if (-not $hit) { throw "no Node 20 zip for $plat" }
    $expected = $hit.Matches[0].Groups[1].Value.ToLowerInvariant()
    $name = $hit.Matches[0].Groups[2].Value
    & curl.exe -fsSL -o "$zip" "https://nodejs.org/dist/latest-v20.x/$name"
    if ($LASTEXITCODE -ne 0) { throw "node zip download failed" }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    if ($actual -ne $expected) { throw "node zip checksum mismatch" }
    Expand-Archive -LiteralPath $zip -DestinationPath $extract
    $inner = Get-ChildItem -LiteralPath $extract -Directory | Where-Object { Test-Path (Join-Path $_.FullName "node.exe") } | Select-Object -First 1
    if (-not $inner) { throw "staged zip has no node.exe" }
    if (Test-Path -LiteralPath $live) { Move-Item -LiteralPath $live -Destination $backup }
    Move-Item -LiteralPath $inner.FullName -Destination $live
    if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Recurse -Force }
  } catch {
    if (-not (Test-Path -LiteralPath $liveNode) -and (Test-Path -LiteralPath $backup)) {
      if (Test-Path -LiteralPath $live) { Remove-Item -LiteralPath $live -Recurse -Force -ErrorAction SilentlyContinue }
      Move-Item -LiteralPath $backup -Destination $live
    }
    $staged = Get-ChildItem -LiteralPath $extract -Directory -ErrorAction SilentlyContinue |
      Where-Object { Test-Path (Join-Path $_.FullName "node.exe") }
    if (-not $staged) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
    throw
  }
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
  $env:PATH = "$live;$env:PATH"
}
```

再次检查 `node -p "process.versions.node"`；主版本仍 < 20 则停止。

## 生成报告

输入可为 `.npu-rep`，或经典 cann-rep / `.rep` 工程报告（脚本侧按内容识别）。

```bash
node <script> <input.npu-rep> -o <output.html> [--name <title>] [--en|--zh]
```

Windows 上一旦 `node` 在 `PATH` 中，参数相同（`node.exe` 可直接跑 `.mjs`，启动不依赖 shebang）。

- 默认标题：省略 `--name` 时用输入文件名
- 默认语言：省略 `--en` / `--zh` 时为中文（`zh-CN`）；`--en` 英文，`--zh` 中文
- 输出为一份交互式 HTML；可用浏览器打开（`file://` 可用）
- 不是 MSTT 打包路径——MSTT 嵌入 Vue 库；本技能仅用于分享/CI

## 示例

```bash
node "$HOME/.cache/npu-rep-html/1.5.5/npu-rep-html.mjs" ./op.npu-rep -o ./op-report.html --en
```

```powershell
$script = Join-Path $env:LOCALAPPDATA "npu-rep-html\1.5.5\npu-rep-html.mjs"
node $script .\op.npu-rep -o .\op-report.html --en
```

实际执行时使用刚解析到的标签，不要硬编码 `1.5.5`（若已有更新的 release）。

## 成功之后

告知用户输出文件的绝对路径，并说明可用浏览器打开。
