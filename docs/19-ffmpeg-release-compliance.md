# FFmpeg 发布合规清单

状态：未完成。本文只固化发布前的可验证材料和阻塞项，不表示当前安装包已经完成 LGPL 分发合规。

## 当前实现边界

- `dogsc_capture` 通过 vcpkg 的动态 `x64-windows` 路径链接 `avcodec`、`avformat`、`avutil`、`swresample`、`swscale`。
- ProRes 422 Standard 使用 FFmpeg `prores_ks` 与 MOV mux；MF remux 是分片合并优先路径，FFmpeg remux 是兜底路径。
- Tauri sidecar 当前会随安装包携带 FFmpeg DLL；构建产物不是本清单的源码或合规材料。
- 禁止复制参考仓库中 Gyan GPLv3 FFmpeg/ffprobe 二进制、构建脚本或发布布局。

## 发布前 REQUIRED 材料

- [ ] 锁定 vcpkg registry revision、triplet、manifest 依赖版本和完整 configure 参数。
- [ ] 记录最终安装包实际携带的每个 FFmpeg DLL 文件名、版本、SHA-256 和依赖关系。
- [ ] 获取与这些 DLL 完全对应的 FFmpeg 上游源码归档。
- [ ] 保存本次构建使用的补丁、差异和可复现构建说明；当前仓库尚未包含这些材料。
- [ ] 保存 FFmpeg 及其实际依赖库的许可证、版权和 NOTICE 文本。
- [ ] 证明动态链接、DLL 可替换性和用户对 FFmpeg 进行调试/修改的路径没有被应用许可或安装器阻止。
- [ ] 在安装包、关于页、EULA 或下载页提供适当的 LGPL/FFmpeg 归属与源码获取说明。
- [ ] 对 ProRes、AAC 以及目标发行地区的专利和商业分发风险进行独立法律审查。
- [ ] 在干净 Windows 环境安装并验证：应用启动、sidecar 加载 DLL、替换 DLL 的失败路径、卸载清理和源码/NOTICE 获取路径。

## 建议的证据采集命令

以下命令只能在最终依赖、构建和安装包锁定后执行；输出应保存到发布归档，不把机器临时路径当作合规证据。

```powershell
Get-ChildItem .\apps\tauri\src-tauri\target\release\bundle\nsis -File |
  Get-FileHash -Algorithm SHA256

Get-ChildItem .\apps\tauri\src-tauri\binaries -Filter '*.dll' -File |
  Select-Object Name, Length, LastWriteTime |
  Get-FileHash -Algorithm SHA256

vcpkg list
```
`tools/release/collect-ffmpeg-manifest.ps1` 会从 sidecar 目录生成 DLL 文件名、大小和 SHA-256 清单；传入 `-InstallerPath` 时同时记录 NSIS 安装包哈希。没有传入源码归档时，清单明确保留 `sourceArchiveRequiredBeforeRelease: true`，不能作为完整合规材料替代品。

FFmpeg 版本、配置参数、源码归档哈希和补丁哈希必须与发布归档中的机器可读清单相互对应；只记录 DLL 文件名不足以完成核验。

## 当前结论

当前本机已取得与 vcpkg FFmpeg `n9.0.2` port SHA-512 相符的源码归档，并已计算 SHA-256；对应 vcpkg port 的 14 个补丁也已逐个计算 SHA-256。当前 manifest 仍是本机发布工作目录证据，源码、补丁、许可证文本、构建复现说明、DLL 可替换验证和法律审查尚未进入最终发布包闭环。因此 `18-feature-matrix.md` 的 Release/FFmpeg 项继续标记为 partial，安装包不得标记为最终合规发布版。
