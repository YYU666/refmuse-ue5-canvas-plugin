# Refmuse Canvas for Unreal Engine 5

在 Unreal Editor 的停靠面板中打开本机 Refmuse 无限画布，从正在编辑的画布项目中选取图片或音频，直接保存到 UE 内容浏览器。Agent 也可以通过画布的结构化接口检查当前项目、创建节点和连线。

这是 Win64 编辑器插件，版本为 `0.1.0-milestone1`。已核对配套画布 **v1.1.0**。工作台单独运行，插件连接用户指定的本地端口。[下载 Refmuse 画布](https://github.com/YYU666/comfyui-infinite-canvas/releases/tag/v1.1.0)

## 安装与连接

1. 准备可编译 C++ 插件的 Windows UE 工程及对应版本的 Visual Studio C++ 工具链。在项目根目录执行：

   ```powershell
   git clone https://github.com/YYU666/refmuse-ue5-canvas-plugin.git Plugins/RefmuseCanvas
   ```

2. 用该项目对应的 UE 版本编译 Editor 目标，在编辑器的插件管理器中启用 **Refmuse Canvas**。编辑器二进制模块须与引擎版本和 BuildId 匹配。
3. 启动本机 Refmuse 工作台并记下实际地址。在 UE 选择 **Window → Refmuse Canvas**，输入 `http://127.0.0.1:<端口>`，点击 **Connect / Reconnect**，再选择画布项目。也可在启动编辑器前设置 `REFMUSE_CANVAS_URL`。

## 把画布素材带到 UE

在画布选中一个已存入项目的素材节点，按 **Ctrl+Shift+C** 或点击 **Copy Selected Canvas Asset**。在 UE 内容浏览器打开目标文件夹，右键空白处选择 **Paste Canvas Asset**。插件核对来源项目和文件 SHA-256，将新资产保存到当前内容目录。

| 素材 | UE 资产 | 当前范围 |
| --- | --- | --- |
| PNG、JPEG | `Texture2D` | 每边最多 4096 像素 |
| WAV | `SoundWave` | 16 位 PCM，单声道或立体声，8–192 kHz |

每次复制一份已保存的素材，上限 16 MiB；复制记录在同一编辑器会话内有效两分钟。画布 v1.1.0 的视频编辑和 3D 加工可在内嵌页面使用。GLB、MP4 的 UE 原生粘贴尚未提供。

## Agent 接口

使用 PowerShell 7，在本仓库根目录执行以下只读示例。将地址换成 UE 面板显示的地址，将 ID 换成已打开的画布项目 ID。

```powershell
$origin = 'http://127.0.0.1:<端口>'
$projectId = 'prj_...'
$tool = './scripts/Invoke-CanvasPageTool.ps1'
$page = & $tool -CanvasOrigin $origin -Mode Current -ProjectId $projectId
$target = @{
    CanvasOrigin = $origin
    ProjectId = $page.projectId
    ExpectedSessionId = $page.sessionId
    ExpectedEpoch = $page.epoch
}
(& $tool @target -Tool canvas_get_state).result
```

创建、连接和修改节点沿用工作台的页桥协议。原生资产操作可在启动编辑器时显式开启 `-RefmuseCanvasAssetCommands`，再用 `scripts/Invoke-CanvasAssetCommand.ps1` 调用；仅在自己信任的本地项目中启用。脚本使用编辑器日志中当前会话的 `Asset command inbox`。[画布 Agent 手册](https://github.com/YYU666/comfyui-infinite-canvas/blob/v1.1.0/docs/AGENT_MANUAL.md)

## 兼容记录与测试

插件原工程在 Win64 的 UE **5.7.4**（CL 51494982）与 **5.8.4**（CL 58902052）验证过内嵌画布读写、项目重开和原生素材导入。**5.9.0 Preview**（CL 60351404）有独立预览版验证记录。其他引擎构建需分别编译验证。

在仓库根目录运行浏览器素材传输测试：

```powershell
node --test tests/fetch-selected-png.test.mjs
```

PCM WAV 解析用例位于 `tests/pcm-wav-parser.test.cpp`，可使用 C++17 编译器单独编译并运行。

## 许可证

插件源码采用 [Apache License 2.0](LICENSE)。Unreal Engine、Refmuse 工作台及用户素材分别按各自条款使用。
