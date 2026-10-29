# 09 · UI 与编辑器

参考：Wessel Mast（Guerrilla），*UIPainter: Tile-Based Rendering in One Draw Call*。

## 1. 分层

```
Editor UI 框架 (Source/Editor/EditorUI)      Signal + 声明式组件树，只给编辑器用
DebugUI       (Source/Runtime/DebugUI)       类 ImGui 立即模式控件，给游戏内调试和开发工具用
Game UI       (项目脚本 + Runtime/GameUI)     游戏自己的 UI，直接调用 UIPainter
──────────────────────────────────────────
UIPainter     (Source/Runtime/UIPainter)     立即模式绘制图元；tile-based GPU 光栅化
  ├ UIText            (Source/Runtime/UIText)            Font 资产、HarfBuzz shaping
  ├ UIIcon            (Source/Runtime/UIIcon)            Icon 资产（SVG 烘焙）、DrawIcon
  └ UIPainterRenderer (Source/Runtime/UIPainterRenderer) GPU 后端：分箱、光栅化、glyph 缓存、窗口合成
```

## 2. UIPainter

- 立即模式 API：`painter.FillRoundedRect(rect, radius, brush)`、`StrokePath`、`DrawText(shapedRun, …)`、`DrawImage(textureHandle, …)`、`PushClip`、`PushTransform`、`PushLayer(opacity, blur)`。
- 每帧把图元记录进命令流 → GPU compute 按 tile（例如 16×16）分箱（binning）→ 每个 tile 只处理覆盖它的图元，在 shader 中计算解析式覆盖率（SDF / 曲线）→ 输出到 UI 目标。
  没有三角化，也没有 overdraw，抗锯齿是解析式的。
- 文字：HarfBuzz 负责 shaping（CJK、复杂文字、emoji），字形的二次贝塞尔轮廓直接交给 tile 光栅化；小字号（≤ 约 24px）使用光栅化后缓存的 glyph atlas，保证清晰和性能。
- Image 图元：SceneView 的输出纹理、编辑器缩略图、视频帧，都通过 bindless 索引引用。
- 输出到 SDR 或 HDR 目标；HDR 时按「UI 白点 nits」缩放。
- Server Target 不链接 UIPainter 的 GPU 后端（01）。

**实现现状**：模块分四块。`UIPainter` 只有记录端，只依赖 Core，没有 GPU 代码，所以 Server 可以链接它；GPU 后端在 `UIPainterRenderer`；文字在 `UIText`；图标在 `UIIcon`（同样没有 GPU 代码）。

- 记录（`UIPainter`）：
  - `Begin(commands, width, height, scale)` 和 `End` 之间的调用都追加到 `UICommandList`；不在录制中的调用直接忽略，`End` 弹出没配对的 push 并报告。坐标是 painter 单位（设备像素除以 scale），Y 向下；颜色是直通（straight）线性 RGBA。
  - 图元：`FillRect`、`FillRoundedRect`/`StrokeRoundedRect`（四角各自的 `UICornerRadii`）、`FillEllipse`/`StrokeEllipse`、`DrawBoxShadow`（高斯模糊的圆角矩形）、`FillPath`（NonZero/EvenOdd）、`StrokePath`、`DrawLine`、`DrawText`、`DrawImage`；栈：`PushClip`（可带圆角）、`PushTransform`（2D 仿射）、`PushLayer(opacity, blur)` 或 `UILayer`（含 `BackdropBlur`，毛玻璃）。
  - Brush：Solid、Linear、Radial、Sweep；渐变在预乘线性空间插值，最多 `MaxStops` 个 stop。
  - `UIPath`：直线和二次曲线组成的轮廓；三次曲线和圆弧在添加时转成二次曲线（误差 1/4 单位）。描边拆成线段和二次曲线 piece 加圆盘 / 多边形（join、cap），同一条描边的 piece 属于一个 group，覆盖率先合并再混合一次；相接的端点带「joined ends」位，不淡出，所以 piece 之间没有接缝。
  - 命令流（`UICommandList`，GPU 结构和 `Language/Shaders/Passes/Ui/UiTypes.mshader` 对应）：每条命令 80 字节（头、brush、clip、transform、group、带抗锯齿边距的设备空间包围盒、11 个数据字），种类有 `RoundedRect`、`Ellipse`、`BoxShadow`、`StrokeLine`、`StrokeQuad`、`Disk`、`Polygon`、`Outline`、`Glyph`、`AtlasGlyph`、`Image`、`Canvas`、`Backdrop`；brush、渐变 stop、transform、clip、曲线和 outline 字都在共享表里，按索引引用，重复的 solid brush 和不变的 transform 会复用。
  - Outline 记录：头之后是 `BandsY` 条横带和 `BandsX` 条竖带，每条列出穿过它的曲线，像素只测自己横、竖射线所在带的曲线；曲线在 x、y 极值处切开，保证单调。
  - Layer 是独立的 canvas（index 0 是目标本身），pop 时在父 canvas 里写一条 `Canvas` 命令合成；有 backdrop blur 时再写一条 `Backdrop`。
  - `UICommandList::Append` 把另一份列表画在上面（索引搬到合并后的表），用于叠加层。
- GPU 后端（`UIPainterRenderer`，`UIPainterSubsystem` + `UIPainterFrameFeature`，在 `UiFrameStage`，SceneView 之后）：
  - 每个 OS 窗口一个 `UIWindowCanvas`（`CreateCanvas(surface)`；`UIPainterSubsystem::MainWindow` 打开时自动为主窗口建一个）。它有两个 painter：`GetPainter` 录这一帧的 UI，`GetOverlayPainter` 录在它上面的内容（调试叠加层、工具）；两份命令列表轮换，一份录制时另一份给 render job。
  - `Extract` 取走录好的列表并解析字形；`AddPasses` 对每个 canvas：命令的 tile 范围在 CPU 上算好，GPU 上按 16×16 tile 做 Count → Scan → Scatter 分箱，然后每个 tile 一个 16×16 线程组按绘制顺序遍历分到它的命令，逐像素算解析覆盖率（形状用 SDF，outline 用带内射线绕数），在寄存器里混合，每个 tile 只写一次。没有三角化，也没有 overdraw。
  - 根 canvas 直接光栅化进窗口的 swapchain 图像（输出声明为 overwrite，不读旧内容）；layer 先画进各自的 `RGBA16Float` 纹理，在那里做可分离高斯模糊（`UI Blur`），再由父 canvas 的 `Canvas` 命令合成；backdrop blur 把父 canvas 在该处切成前后两段。
  - 编码：SDR 输出 sRGB；HDR10 输出 PQ，UI 白点取 `RenderSurface::GetUiWhiteNits()`（`r.UiWhiteNits`，0 表示跟随显示器的 SDR 白点）。
  - Image：bindless 的 texture view 描述符索引，或 frame slot（`RendererSubsystem::AllocateImageSlot`，SceneView 每帧把输出绑上去，后端按 render graph 纹理读取，07 §5）。
  - 7 个全局 compute shader（`Marlin.Passes.Ui.Count/Scan/Scatter/RasterSurface/RasterLayer/RasterGlyphs/Blur`）还没就绪时，只把窗口清成 `ClearColor`。
  - `GetLastFrameStats`（canvas、命令、tile、命令-tile 对、新进 atlas 的字形数）给调试叠加层用。
- 字形缓存（`UIGlyphCache`，主线程）：`Resolve` 把 `Glyph` 命令改写成 `Outline`（引用缓存的字形 outline 记录）或 `AtlasGlyph`。轴对齐且 ≤ 24 设备像素/em 的字形走 2048×2048 atlas（4 个亚像素相位，按 shelf 打包，满了整个清空），由 `RasterGlyphs` 用同一套 outline 覆盖率光栅化；其余直接画 outline。
- 文字见 §2.1。
- 图标（`UIIcon`，和 UIPainter 一样没有 GPU 代码）：`Icon` 资产是从 SVG 烘焙好的形状（06 §3.6.1）；`DrawIcon(painter, icon, rect, color, strokeWidth)` 把图标等比缩放、居中放进 `rect`，
  `currentColor` 换成 `color`；`strokeWidth` 大于 0 时所有描边都用这个宽度（painter 单位，即 Lucide 的 absolute stroke width），为 0 时按原宽度随图标缩放。
  每次绘制在 painter 单位里重建路径，所以曲线转二次曲线的误差不随图标大小变化。引擎图标在 `Content/Icons`。
- 尚未实现：彩色字形（COLR）的绘制（`UIShapedRun::HasColorGlyphs` 已经标出，但还没有按层绘制）、编辑器缩略图和视频帧图片、多窗口的实际使用（目前只有主窗口）。

### 2.1 文字（UIText）

- `Font` 资产：完整保存的 sfnt 文件（`Data`）和 collection（.ttc）中的 `FaceIndex`；HarfBuzz 直接从 `Data` 读表，不预先光栅化。`FontSource` 和导入、转换器在 `Developer/FontPipeline`。
- `UITextSubsystem`（Config `Engine.UIText`）：
  - `RegisterFont` 把 Font 资产注册成 `UIFontFace`，字体加载完之前这个 face 什么都不画。
  - `Shape(text, {Faces, Size}, shaped)`：用 HarfBuzz shaping 一行 UTF-8（文字系统、连字、字距、组合符号），按字符在 style 的 face 列表里回退，每段同一 face 的文字一个 `UIShapedRun`；没有任何 face 有的字符显示第一个 face 的缺字字形。shaping 在文字变化时做一次，不是每帧做。
  - `GetGlyphOutline`：字形轮廓（em 单位，Y 向下，三次曲线转二次），给字形缓存用。
  - shaping 和取轮廓是线程安全的。
  - 默认字体：`DefaultFonts`（Inter，然后 Noto Sans）和 `MonospaceFont`（JetBrains Mono，表格、日志、统计用），启动时加载；`GetDefaultFaces`、`GetMonospaceFaces` 返回回退顺序。引擎字体在 `Content/Fonts`。
- `DrawText(painter, shapedText, origin, brush)` 画一整行的所有 run。

## 3. DebugUI（Runtime）

在 UIPainter 之上做一层类似 ImGui 的立即模式封装：`DebugUI::BeginWindow`/`EndWindow`、`Button`、`Slider`、`Plot`、`BeginTable`、`TreeNode`。
用来做游戏内调试面板、统计数据（stat）和 CVar 编辑器；**编辑器本身不使用 DebugUI**。Shipping 不链接（Game Target 只在非 Shipping 配置里加入 `DebugUI`）。

**实现现状**（`Runtime/DebugUI`）：

- 控件（`DebugUI::`）：`BeginWindow`/`EndWindow`（位置只在第一次生效，拖标题栏移动；高度取上一帧内容；可带关闭按钮；`NoTitleBar`、`NoInput` 标志）、`Header`、`Text`、`Value`（左标签右值）、`Separator`、`Spacing`、`Button`、`Checkbox`、`Slider`（float、int）、`InputText`（Enter 提交）、`Plot`（折线图加淡网格）、`BeginTable`/`TableCell`/`TableButton`/`EndTable`、`TreeNode`/`TreePop`、`Banner`（顶部的警告条）、`PushId`/`PopId`。
- 控件按窗口内的 label（`##suffix` 区分同名而不显示）和 `PushId` 区分；主线程调用，立即绘制，返回本帧用户的操作；没有 `DebugUISubsystem` 或这一帧还没开始时什么都不做、返回 false。`WantsMouse`、`WantsKeyboard` 告诉游戏不要用这一帧的鼠标、键盘。
- 外观（`DebugUIStyle`）：参考 Metal Performance HUD，半透明的紧凑面板、等宽字体的行、细线图。
- `DebugUISubsystem`（Config `Engine.DebugUI`）：每帧用主窗口的输入在主 canvas 的 overlay painter 上开始 DebugUI 的上下文（所以总在游戏 UI 之上），然后画调试叠加层、最近的警告和控制台窗口。
  - 叠加层：CVar `ui.DebugOverlay`；命令行 `-DebugOverlay` 或 F1 打开。
  - 控制台窗口：CVar `ui.DebugConsole`，F2 切换。命令行用 `Console::Execute` 执行并显示输出；下面是可按名字过滤的 CVar 树，修改走 `Set(..., ConsoleSetBy::Console)`。
  - 文本框占着键盘时不响应 F1、F2。
  - Warning、Error 日志以 banner 显示 `BannerSeconds`（默认 8）秒。
- 叠加层的分段是扩展点 `DebugOverlaySection`（`MCLASS(Abstract, ExtensionPoint)`），通过反射找到，按 `MCLASS(Priority = N)` 从高到低画；分段对象和叠加层一样长寿，用来保存随时间绘制的历史。内置：
  - `FRAME`（Priority 300）：帧率、帧间隔和历史图；
  - `GPU`（200）：设备、主 surface、RenderGraph 时间戳得到的 GPU 帧时间和最耗时的 pass、显存；
  - `UI`（100）：UIPainter 的命令、tile、glyph atlas 和 outline 统计。
- 尚未验证：手按 F2 切换控制台（只用 `-ExecCommands=ui.DebugConsole true` 验证过窗口）。

## 4. 编辑器 UI 框架

**决策**：细粒度 Signal 加声明式组件树，用 C++ 编写。

### 4.1 Signal 与组件

```cpp
UIElement MaterialParameterRow(const ParameterBinding& binding)
{
    Signal<bool> expanded = UseState(false);
    return Row({
        Toggle(expanded),
        Label(binding.DisplayName),
        Show(expanded, [&] { return PropertyEditor(binding.Property); }),
    }).Padding(Theme::Spacing.Small);
}
```

- 组件函数**只执行一次**，用来构建元素树。属性绑定到 `Signal<T>` 或 `Computed<T>`，Signal 变化时只更新依赖它的节点（布局、绘制标脏），没有虚拟 DOM diff。
- 控制流组件：`Show`、`For`（带 key 的列表）、`Switch`、`VirtualList`（百万级行数，只实例化可见范围）。
- 布局：Flex 加 Grid，分「测量、排列」两遍，只重排标脏的子树。
- 每帧：事件分发 → Signal 传播 → 布局 → 遍历元素树，把绘制命令写入 UIPainter → 提交。

**实现现状**（`Editor/EditorUI`）：

- `Signal`/`Computed`/`CreateEffect`：Computed 惰性求值、无毛刺；Effect 批量到 `Reactive::Flush`（每帧的 Signal 传播阶段）执行，每次运行重新收集依赖。所有响应式对象都挂在 owner 树上，随 owner 一起释放；Effect 和 Computed 每次运行前先释放上一次创建的东西。句柄带世代号检查。另有 `UseState`、`Untrack`、`OnCleanup`。
- 元素树：`Box`/`Row`/`Column`/`Grid`/`Text`，控制流 `Show`、`For`（按 key 复用行）、`Switch(Match(...), ..., Default(...))`（`Function` 只能移动，所以 `Switch` 用变参而不是 initializer_list）。属性可以是常量、`Signal`、`Computed` 或函数；绑定的属性变化时只把布局标脏到根。
- 布局：Flex（Grow、Shrink、Basis、Justify、Align、Gap、Margin、Padding）和 Grid（固定、Auto、Fraction 轨道，跨行列，自动放置），先测量再排列，按节点缓存。显式的 Width/Height 优先于 Stretch。
  - `AutoFillColumns(最小宽度)`：Grid 放下尽量多的等宽列，每列不小于这个宽度（CSS `repeat(auto-fill, minmax(w, 1fr))`），用于会换行的缩略图。
  - `Visible(false)`：移出布局、绘制和命中测试，但保留子树和状态（CSS `display: none`），用于切换标签页而不重建内容。
  - `Absolute(offset)`：移出流，按内容大小放在父元素内容区原点加 offset 处，画在流内元素之上（CSS `position: absolute`），用于弹出菜单和覆盖层。
  - `Translate`：只移动绘制和命中测试，不重新布局，用于拖拽和滚动。
  - `Scale`：绕元素左上角缩放绘制和命中测试，不重新布局，用于弹出动画。
  - `OnPaint`：自定义绘制，在背景之上、子元素之下（图标、指示线）。
- 动画（`UIAnimation.h`）：
  - UI 时钟：`EditorUISubsystem` 每帧在 Flush 之前 `UIAnimation::SetTime`；`UIAnimation::Now()` 让读它的 Computed、Effect、绑定属性每帧更新。
  - `Tween<T>`（float、`Vector2`、`LinearColor`）：句柄，`Set(目标)` 从当前值按 `UITransition`（时长加缓动函数）缓动过去，`Snap` 直接跳过去。
    `Get` 只在运动期间跟踪时钟，停下后就不再每帧触发。
  - `Animate<T>(source, transition)`：让一个响应式的值（如悬停颜色）在每次变化时缓动过去，返回可以直接当属性值用的函数。
  - 缓动曲线的数学在 Core（`Math/Easing.h`：`EasingFunction` 函数指针，`Easing::Linear / EaseInCubic / EaseOutCubic / EaseInOutCubic / EaseOutBack`），
    编辑器和游戏共用；编辑器在代码里直接写曲线（`UITransition { Duration, Easing }`），可以传自己的函数。
  - 编辑器里的用法：菜单弹出（淡入、下落几个单位、从左上角放大并略微回弹）；菜单栏、菜单项、窗口按钮、标签页的悬停颜色淡入淡出；
    标签页换位、关闭、拖拽松手时用 FLIP 方式滑到新位置（`EditorShellSubsystem` 的标签栏每帧比较布局位置的变化）。
  - 时钟、`Tween` 和 `Animate` 建在 Signal 上，只属于编辑器。游戏 UI（`Runtime/GameUI`，尚未实现）用自己的方式驱动动画，
    曲线做成资产（由美术调），求值用同一套 Core 缓动数学。
- 通用控件：
  - `UIWidgets.h`：`IconView(token, color, size, strokeWidth)` 画主题里的图标；`TrackHover`、`TrackPress` 把元素的悬停、按下状态变成 Signal；
    `WithChildren`、`FadeIn`（悬停高亮淡入淡出）。
  - `UIControls.h`（样式全来自主题 token，对应设计稿的组件表）：`Button`（Secondary / Primary，可禁用）、`IconButton`、`ToolbarButton`（图标在上、文字在下）和
    `ToolbarSeparator`、`TextField`（单行：点击获得焦点、输入、Backspace / Delete、方向键、Home / End、`Primary`+V 粘贴（Windows、Linux 是 Ctrl，macOS 是 ⌘，12 §1.5）；占位文字、左侧图标、非空时右侧 ✕ 清空；
    光标停住时闪烁，文字超出时跟着光标滚动）、`Chip`（筛选开关）、`Slider`、`SegmentedIcons` / `SegmentedLabels`、`ScrollView`（滚轮滚动，右侧细滚动条）、
    `Section`（可折叠分组）、`ShowContextMenu`（`UIMenuItem`：图标、文字、快捷键提示、可用、分隔线）。
  - 控件接受的 `UIValue` 只能移动，控件内部用 `Computed` 包一层再交给多个 lambda。
- 指针事件的 `Clicks`：同一元素上 0.4 秒内、4 个单位之内的再次按下计为第 2 次（双击），按下、松开、点击都带着它。
- 弹出层：`UIWindowRoot::ShowPopup(位置, 构建)` 把内容放在窗口最上层（自己的 owner），点外面或 Esc 关闭，超出窗口时下一帧移回窗口内；
  `ClosePopup` 推迟到下一次布局，所以弹出层里的处理函数可以调用它。`UIWindowRoot::Find(元素)` 找到元素所在的窗口。
- `UIWindowRoot`：每个 OS 窗口一棵树，负责事件分发（冒泡、悬停、按下捕获、焦点）、布局和用 UIPainter 绘制。`EditorUISubsystem`（Editor 作用域）每帧把主窗口的事件交给它，然后 Flush、布局，画进主画布，最后把标题栏区域交给 OS（§4.5）。
- 尚未实现：`VirtualList`（现在的列表和缩略图网格一次建出所有行）、下拉框、数值输入框、多行文本、文本选择和复制、拖动滚动条。

### 4.2 数据模型、Transaction 与 Undo

- 编辑器数据都是反射对象（Source 资产对象、World 中的实体和组件、设置）。
- **所有修改都必须经过 `Transaction`**：`using (auto tx = Transaction::Begin("Move Entities")) { … }`。Transaction 记录反射级别的前后差异，自动进入 Undo 栈，并通知相关的 Signal（Inspector 和大纲视图会自动刷新）。
- Undo 栈按 AssetContext 分开，也有一个全局栈。

### 4.3 Command

- 所有用户操作都是 `Command`（Id、名称、图标、快捷键、可用条件、执行函数），菜单、工具栏、快捷键、命令面板都从这里取。
  快捷键是 `KeyChord`（可以用 `Primary` 修饰键），显示文字由 `PlatformInput::FormatChord` 按平台生成，平台惯例不同的默认值在 Config 里按 `@if Platform` 分层（12 §1.5）。
- Command 同时注册为 Remote 方法（10），所以 MCP 和自动化测试能以和用户完全一样的方式操作编辑器。

### 4.4 Inspector

根据反射自动生成；可以用 `MPROPERTY` 元数据（Range、Unit、EditorCategory、`EditCondition`）调整，也可以按类型或字段注册自定义 `PropertyDrawer`。
有 override 的字段（Prefab、材质实例、Config 层）会高亮显示，并提供 Revert。

### 4.5 窗口、DPI、主题

- OS 窗口、输入、DPI 和显示器信息来自 `Application` 模块（12 §1.3），编辑器 UI 框架在它之上管理窗口和 Docking。
- 支持多个 OS 窗口和 Docking：面板可以拖出成独立窗口；每个 OS 窗口有一个 swapchain。
- 每个显示器有各自的 DPI，窗口跨显示器时重新布局并缩放。
- HDR 合成见 §2。
- 主题：`/Engine/Config/Editor/Theme.mtext`（颜色、间距、字体、圆角、图标映射），支持热重载，项目或用户层可以覆盖。

当前进度：每个 OS 窗口的 swapchain 是一个 `RenderSurface`（Renderer），UIPainter 用 `UIWindowCanvas` 合成整个窗口（§2），painter 的 scale 取 surface 的 DPI 缩放；每个编辑器窗口一棵 `UIWindowRoot`（§4.1）。

- **自绘标题栏**：`WindowFlags::CustomFrame`（Config `Engine.Window.CustomFrame`，`@if Target == Editor` 打开）去掉 OS 标题栏，客户区覆盖整个窗口；OS 仍负责拖动、Aero Snap、边缘拉伸、阴影和最大化动画。
  应用用 `PlatformWindow::SetHitTestRegions` 告诉 OS 哪里是标题栏（`Caption`，拖动、双击最大化、右键系统菜单）、哪里是客户区；Windows 在 `WM_NCHITTEST` 里先判断边缘拉伸，再从后往前查这些区域。
  UI 这边把元素标成 `WindowCaption()`：`UIWindowRoot::CollectHitRegions` 把它们收成 Caption 区域，其中接受按下的元素（有 OnPointerDown、OnPointerUp 或 OnClick）收成 Client 区域，`EditorUISubsystem` 在区域变化时才调用 `SetHitTestRegions`。
  最小化、最大化 / 还原、关闭按钮按 `PlatformWindow::GetFrameControls` 处理（12 §1.5）：系统不画时自绘，分别调用 `PlatformWindow::Minimize`、`Maximize` / `Restore`、`RequestClose`，
  位置和顺序按它给的；系统画时（macOS 的红绿灯）只在那一侧留出它要的区域，标签页从留空之后开始。浮动窗口（§5）同样。
  双击和右键标题栏的行为交给平台（Windows 是最大化和系统菜单），编辑器不自己实现。
- **窗口大小**：`Engine.Window.Maximized` 让主窗口启动时最大化但保留边框（`Fullscreen` 是无边框全屏），还原后回到 `Width`×`Height`；
  `MinWidth`、`MinHeight` 是用户能缩到的最小客户区（96 DPI 下的值，按窗口 DPI 放大；Windows 在 `WM_GETMINMAXINFO` 里换算成整个窗口的大小）。
  编辑器默认最大化，最小 960×540。
- **改变窗口大小**：swapchain 在下一帧结束时才跟上窗口大小，中间新露出的区域没有内容。两个措施：
  - 每个编辑器窗口的 canvas 用主题的 `Window.Background` 清屏，布局还没跟上的部分是背景色而不是黑色；
  - 用户拖窗口边缘时（`WM_ENTERSIZEMOVE` 到 `WM_EXITSIZEMOVE`），窗口线程在每次 `WM_SIZE` 里等这个大小的一帧被 present（Renderer 调
    `PlatformWindow::NotifyPresented`），最多 100 ms；等待期间照常处理其他线程发来的消息，不会和主线程互相等死。
- **多个 OS 窗口**：`EditorUISubsystem::OpenWindow` 开一个自绘标题栏的窗口（`UIPainterSubsystem::CreateWindowCanvas` 给它自己的 surface），返回它的 `UIWindowRoot`；`CloseWindow` 在这一帧的 Update 结束时销毁。
  每帧先把各窗口的事件分给各自的树，整体 Flush 一次，再逐个窗口布局、绘制、更新标题栏区域。`FindWindowAt` 按桌面坐标找窗口（后开的优先，它们在上面），`GetPixelsPerUnit` 给出窗口的像素 / painter 单位。目前只有浮动 Dock 用它（§5）。
- 尚未实现：跨显示器时的重新布局、Win11 的 Snap Layouts 弹窗（需要把最大化按钮报成 `HTMAXBUTTON`，并自己模拟非客户区的悬停和点击）。

主题的实现现状（`EditorTheme`、`EditorThemeSubsystem`，`Editor/EditorUI`）：

- `Config/Editor/Theme.mtext` 按 Config 层叠读取（06 §1.4，段名 `Editor/Theme.Colors`、`Editor/Theme.Metrics`、`Editor/Theme.Icons`），后面的层**按单个 token** 覆盖，所以项目或用户只写要改的几个。
- 默认值是一套紧凑的深色工具界面：11–12 px 文字，强调色 `#F0A531`。
- `Colors`：值是 sRGB 的 `"#RRGGBB"` / `"#RRGGBBAA"`（alpha 线性），或另一个颜色 token 的名字（带点时加引号）。引用在所有层合并之后才解析，所以改调色板（`Gray0…18`、`Orange`…）就能改所有角色色（`Panel.Background`、`Text.Primary`、`Accent.Normal`、`Tab.*`、`MenuBar.*`…）。
- `Metrics`：数字，painter 单位（`Spacing.*`、`Radius.*`、`FontSize.*`，以及 `TitleBar.*`、`Tab.*`、`MenuBar.*`、`StatusBar.*` 等各部件的尺寸）。
- `Icons`：图标名到 Icon 资产的链接（`FolderOpen = <folder-open#UUID>`，§2），`EditorTheme::GetIcon` 第一次用到时异步加载，加载完之前什么都不画。
- 嵌套 block 用点连成 token 名：`Text = { Primary = … }` 是 `Text.Primary`。token 是开放的，插件可以加新 token，不用改引擎。
- UI 代码按名字取：`Box().Background(ThemeColor("Panel.Background")).Padding(ThemeMetric("Spacing.Medium"))`。两者都是函数对象，`UIValue` 把它们当响应式值，读 token 会跟踪主题的版本 Signal，重载后绑定的属性在下一次 Flush 更新。
- 缺失、循环引用或格式错误的颜色是品红色，缺失的 metric 是 0，每个 token 只警告一次。
- `EditorThemeSubsystem` 在 Initialize 时加载并设为当前主题；`EditorUISubsystem` 依赖它。控制台命令 `Editor.ReloadTheme` 重新读 Config 并重载（Config 还没有文件监控，06 §1.4）。
- 尚未实现：主题里的字体、多套主题之间切换（目前靠 `/User/Config` 覆盖）。

### 4.6 启动画面（Splash）

**决策**：编辑器启动时先显示一个原生的小窗口，直到主窗口能画出完整的第一帧；冷启动时既不让用户看到画了一半的编辑器，也告诉用户在等什么。

- **为什么需要**：转换结果按需生成、存进 GlobalStore（06 §3），编辑器启动时不等它们。缓存为空时，UIPainter 自己的 compute shader、默认字体和等宽字体、
  主题图标都要现场转换，在这之前主窗口是空白的，或者缺字、图标一个个冒出来。
- **怎么画**：平台层的原生窗口（`PlatformSplash::Show / SetStatus / Hide`，Windows 上用 GDI 画），不经过 Renderer 和 UIPainter，
  因为它要等的正是 UIPainter 的 shader。进程启动后尽早显示（第 4 步之后，01 §2），覆盖引擎初始化、拉起 Indexer 和 `MarlinGlobalStore`、创建设备。
  - 外观按设计稿 `Splash Screen`：720×400 的深色卡片（`#161616`，`#2A2A2A` 边框，Windows 11 上由 DWM 画圆角和边框），左边 4 px 的强调色竖条（`#F0A531`）；
    左上 logo 和「MARLIN」，右上版本和构建号（`EngineVersion`、`Build <EngineId>`）；中间大标题「Marlin Engine」和「Opening project <项目名>」；
    底部一行当前任务、计数、百分比，3 px 进度条，再下面「Step k of 2」和版权。尺寸按主显示器 DPI 缩放，窗口居中、可拖动、在任务栏显示。
  - 字体用引擎自带的 Inter 和 JetBrains Mono（设计稿的 IBM Plex 不在引擎里）：从 `<EngineRoot>/Content/Fonts` 以进程私有字体加载，
    打包后的引擎没有源文件，退回 Segoe UI 和 Consolas。
  - 接口：`PlatformSplash::Show(SplashContent)`（不变的文字、字体文件、`Tick` 回调）、`SetStatus(SplashStatus)`（项目名、任务、计数、阶段、进度）、`Hide`。
    平台层只负责画，文字和进度由 `Splash` 模块（`Runtime/Splash`）在 `Tick` 里算好交给它。
  - 目标通过 `LaunchDesc::Splash`（`StartupSplash { Show, Hide }`）接入：Editor 的 `main` 传 `Splash::GetStartupSplash()`；
    Engine 在第 4 步之后调 `Show`，`Shutdown` 时调 `Hide`，以防启动失败时它还开着。`-Headless` 或 `-NoSplash` 不显示。
  - Splash 在平台的**窗口线程**上（12 §1.5），不在引擎主线程上：第 4–10 步主线程停在同步代码里（Subsystem 的 `Initialize`、等 Indexer 的管道、创建设备），
    不能靠主循环刷新它，否则它和主窗口一样卡住、拖不动。`PlatformSplash::Show` 在窗口线程还没启动时先启动它（macOS 上窗口线程就是进程主线程，本来就在）。
    Splash 在窗口线程上用定时器读下面的进度快照、重画，不调用引擎的其他部分。

**决策**：进度按**第一帧需要的东西**算，不预估工作量。实际耗时 = 需要什么 × 缓存里有没有；需要什么在每条启动路径上都一样
（直接 `-Project` 打开、从项目浏览器重启、Game 以后接入），缓存命中与否只在定位时才知道，命中的项立即完成。
所以 Splash 不区分这些路径：从项目浏览器进来时 UI shader 已经在 GlobalStore 里，同样的进度一闪而过；直接打开冷项目时停在「编译中」的那几项上。

- **启动段**（第 4–10 步，主循环之前）：`StartupProgress`（Engine 模块，开放注册，没有写死的阶段枚举）记录当前步骤和它的状态文字。
  - `SubsystemCollection::Initialize` 规划好顺序后把这批的个数加进总步数，每个 Subsystem `Initialize` 之前写「Initializing <Subsystem>」，
    之后步数加一；Engine 在每个 `Start` 之前写「Starting <Subsystem>」。所有作用域共用（Editor Subsystem 同样计入），Splash 显示为「k / N」。
  - 一次耗时可能较长的同步操作由负责它的 Subsystem 自己写一条状态，覆盖上面那条：「Starting asset indexer」「Starting GlobalStore」「Creating GPU device」。
  - 写入是线程安全的（Splash 线程读），在没有 Splash 的进程里是空操作的开销。
- **资源段**（主循环开始后，主窗口隐藏）：资产表统计 Splash 显示期间请求的 **`IoPriority::Critical` 加载**（`Assets::GetCriticalProgress`）：
  请求数、完成数（成功和失败都算完成），以及每个未完成项的阶段和名字：
  - `Locating`：在等 Indexer 定位（第一次全量扫描完成前 `Asset.Locate` 会排队，06 §2），状态行「Waiting for the asset indexer」；
  - `Converting` / `Compiling`：Indexer 没命中、派给了 worker（`Asset.Converting` 通知，06 §2，客户端调 `Assets::NoteConverting`），
    状态行显示「Compiling Marlin.Passes.Ui.RasterSurface」或「Converting NotoSans-Regular」，转换中的项优先显示；
  - `Loading`：结果已定位，正在从 GlobalStore 读，状态行「Loading assets」；全部加载完、只等就绪检查时是「Preparing the editor」。
  - Critical 加载引用的资产以同样的优先级加载，所以也计入。
  - 判断标准是优先级而不是「Splash 期间的所有加载」：Asset Browser 和恢复的资产编辑器会请求项目 Content，Splash 不等它们（见下面「不做什么」）。
    因此 UI 启动必须的资源都用 Critical 加载：`ShaderLibrary` 的全局 shader、`UITextSubsystem` 的默认字体和等宽字体已经是；
    主题图标改成 Critical（`EditorTheme::GetIcon`）。Game 的启动关卡（`WorldManagerSubsystem`）本来就是 Critical，以后接入时不用改。
  - 图标在第一次用到时才请求，启动前列不出完整清单，所以不预先枚举：主循环在主窗口隐藏期间照常跑（布局、录制 UI、渲染到隐藏窗口），
    第一帧用到的图标在这时请求，自然计入请求数。
  - 主窗口以 `WindowFlags::Hidden | Maximized` 创建：隐藏时已经是最大化后的客户区大小（Windows 上自绘标题栏的窗口先占满工作区），
    所以隐藏期间画的帧尺寸就是显示时的尺寸；`ApplicationSubsystem::ShowMainWindow` 再最大化显示，还原时回到 `Width`×`Height`。
    没有 Splash 时 `ApplicationSubsystem` 创建完立即显示，行为和以前一样。
- **Indexer 状态**（`AssetIndex.Status`，06 §2）作为状态行的补充信息：「Indexing 1200/5000」（`FilesRead / FilesToRead`）、
  「Converting 3 (12 queued)」（`ConversionsRunning / ConversionsQueued`）、全局 shader 预热「Compiling shaders 12/40」（`ShadersWarmed / ShadersToWarm`）。
  只显示，不计入进度，也不决定 Splash 什么时候关。
- **进度条**：启动段占前 30%（k/N），资源段占后 70%（完成数 / 请求数）。资源段的请求数会随着第一帧的请求变大，
  进度条只前进不后退（取历史最大值）；计数一栏显示真实计数（「12 / 40」「28 / 31 assets」）。不记录上次启动的总数来预估。
- **什么时候切到主窗口**（`SplashSubsystem`，只在 Splash 显示时创建）：主窗口创建时隐藏。在一帧的 `EndFrame` 满足以下条件，
  并且连续两帧都满足（第二帧是一切就绪后画的完整帧）时，显示主窗口、关闭 Splash：
  - 主循环已经开始；
  - 没有未完成的 Critical 加载；
  - 注册过的就绪检查都通过（开放注册，`StartupProgress::AddReadyCheck`，主线程）：UIPainter 的 7 个 shader 和 pipeline 就绪（`UIPainterSubsystem`），
    每个编辑器窗口没有在等字体的文字（`EditorUISubsystem`，`UIWindowRoot::HasPendingText`）。
  - 某项加载失败不卡住 Splash：它算完成，照常进入编辑器，错误留在日志里。
  - 加载都完成、但某个就绪检查在 `Engine.Splash.ReadyTimeout`（默认 10 s）内一直不通过时（例如 UI shader 编译失败），照样显示主窗口，并警告是哪个检查。
    转换本身多久都等，不设超时。
- **日志**：第一帧之前 `LogSubsystem` 还不在每帧刷新（03 §7），所以启动期的日志会一直留在缓冲里，第一帧后一起出现。
  Splash 显示期间，Splash 的定时器（窗口线程）每 100 ms 调一次 `Log::Flush`，主循环开始后由 `LogSubsystem::EndFrame` 接手；终端和日志文件在启动期也能实时看到。
  在 `LogSubsystem` 加上日志文件的 sink 之前（`LogSubsystem::IsStarted`）不刷新，否则那些记录只到终端、不进日志文件。
- **不做什么**：
  - 不预先转换项目的 Content，其他资产照旧在打开时按需转换。
  - 不等全局 shader 预热全部完成：Indexer 在后台继续编译，Splash 只等编辑器 UI 立即要用的那几个，冷启动尽量短；热启动时全部命中 GlobalStore，Splash 一闪而过。
  - 暂时只有 Editor 用；Game 需要时链接同一个模块即可。
- **出现的时机**：每个编辑器进程启动时一次。没有项目时它之后是项目浏览器；选好项目会以 `-Project` 重启编辑器（01 §5.1），新进程再显示一次。
- 当前进度：Windows 上已实现（`Runtime/Splash`、`Application` 的 `PlatformSplash`、`Platforms/Windows/.../WindowsSplash.cpp`）。
  热缓存下 Sandbox 冷启动（Indexer、GlobalStore 都没在跑）31 个 Critical 加载，第 3 帧切到主窗口。
- 尚未实现：Indexer 状态（`AssetIndex.Status`）的补充显示；macOS、Linux 的 `PlatformSplash`。

## 5. 编辑器交互模型

- **Asset Browser**：常驻面板，数据来自 Asset Indexer 的查询（过滤、标签、Reference Viewer、版本控制状态、缩略图来自 GlobalStore）。
- **资产编辑上下文（AssetContext）**：打开一个资产，就进入该类型对应的编辑上下文（标签页）：
  Level 和 Prefab 是场景编辑上下文；Material 有参数面板、预览视口和 shader 源码视图；Texture、Mesh、Animation、Script 各有自己的编辑上下文。
  每个上下文有自己的 `AssetContextSubsystem`、选择集、Undo 栈，按需拥有 Preview World。
- **视口**：每个视口是一个 SceneView；同一 World 的多个视口共享这个 World 和它的 RenderScene（07 §5）。

当前进度：`Editor` Target（launch module `EditorLaunch`）链接 `AssetManager`、`EditorShell`、`EditorUI`、`SceneEditor`、`UIPainterRenderer`、`World` 和 Game 在非 Shipping 下链接的开发工具（不链接 `DebugUI`）。引擎启动后 `EditorHostSubsystem` 启动 Editor 作用域（01 §3）。

主窗口（`Editor/EditorShell`）：

- **布局**（`EditorShellSubsystem`）：自绘标题栏（每个编辑上下文一个标签页，窗口按钮或系统按钮的留空按平台在左或右，§4.5）→ 菜单栏（右端是构建号；macOS 上没有这一行，菜单在系统菜单栏里，构建号移到标题栏右端，12 §1.5）→ 当前上下文的内容 → 状态栏（左右两个空槽，留给提示和帧号）。
- **编辑上下文**（`EditingContext`）：每个标签页一个，在自己的 owner 下创建，标题是 Signal；内容（`BuildContent`）第一次显示时才构建，切到别的标签页时隐藏而不销毁。
  - 固定上下文由 `PinnedContextType` 创建（`Create`、`GetDisplayName`），启动时每种开一个，排在最前，常驻、没有关闭按钮、不能拖动（`IsPinned`），Window 菜单各有一项。
    Asset Browser（`AssetManagerContextType`，`Editor/AssetManager`）是其中之一；`EditorShell` 不知道它。
  - 资产上下文由 `AssetContextType` 创建：`CanEdit(资产类型)`，`Priority` 高的优先；同一个资产只开一个。
  - 工具上下文由 `ToolContextType` 创建：不针对某个资产，每种最多一个，`GetMenu()` 指定它出现在哪个菜单。
  - 这些类型都是 Provider 扩展点（13 §3），链接了就能被发现，不写注册代码。
  - **模块划分**：`EditorShell` 只放框架（上下文、Dock、菜单）；有自己界面的上下文各占一个 Editor 模块，模块里放它的上下文类型、`DockType` 和加菜单项的 Subsystem，例如 `SceneEditor`、`AssetManager`。小工具可以合在一个模块里。
- **`EditingContextSubsystem`**：标签顺序和当前上下文都是 Signal；`OpenAsset`、`OpenTool`（已经打开就激活）、`OpenContext`（打开一个不对应已打开资产的上下文，例如未保存的场景）、`Activate`、`Close`（关掉当前标签时激活右边的，没有就激活左边的；`CanClose` 可以拒绝）、`Reorder`（不会排到固定的上下文前面）。
  控制台命令 `Editor.OpenAsset <UUID> <名字>`、`Editor.OpenTool <名字>`。
- **标签栏**：按下即激活；拖动超过 4 个单位开始拖拽，被拖的标签跟着指针走，中心越过邻居中心时换位；中键关闭；标签放不下时滚轮横向滚动，新激活的标签会滚进视野。
- **菜单栏**：有哪些菜单来自 Config `Editor.Menus.MenuBar`；菜单项由各 Subsystem 用 `EditorMenuSubsystem::AddItem` 加（文字、快捷键提示、顺序、执行、是否可用、是否显示）。
  点击打开下拉菜单；打开时指针移到别的菜单上就切换过去；点菜单外面关闭。没有菜单项的菜单不会打开。
  - `PlatformMenu::HasGlobalMenuBar()` 为真（macOS）时不画菜单栏：`EditorMenuSubsystem` 在菜单内容或状态（可用、显示、勾选）变化时
    把整个菜单栏做成快照交给 `PlatformMenu::SetMenuBar`，选中菜单项以 `MenuItemActivated { Id }` 事件回到主线程执行（12 §1.5）。
    菜单项的数据（`AddItem`）两种方式共用，扩展菜单的 Subsystem 不用区分平台。
  - 快捷键提示由菜单项的 `KeyChord` 生成（§4.3 的 `Command`），不写死字符串。

Dock（`Dock.h`、`DockSpace`、`DockingSubsystem`，`Editor/EditorShell`）：

- **`DockType`**：Provider 扩展点，按反射类型名称呼（布局里也用这个名字）；`GetDisplayName`、`CanDockIn(上下文)`、`Build(上下文)`。
  插件给别的上下文加 Dock 只要写一个 `DockType`，不用改那个上下文。
- **`DockSpace`**：一个上下文的所有 Dock。上下文在构造时创建它（给出布局名和默认布局），`BuildContent` 返回 `BuildMainArea()`。
  - 由主区域和若干浮动窗口组成，每个区域是一棵树：分割节点（`Row` / `Column`，子节点按比例分）和叶子 Stack（多个 Dock 叠成标签页，一次显示一个）。
  - 每个 Dock 的内容在自己的 owner 下构建一次，移动、浮动都不重建，关闭时释放。
  - **Toolbar**：主区域可以带一个 Toolbar，固定在最上面一行，不能移动、不能关闭；但它右边可以放 Dock（顶行只能横向分割，Dock 只能放进或放到 Toolbar 右边的 Dock 旁边）。Toolbar 里的按钮以后来自 Command（§4.3），现在是空的。
- **交互**：
  - 按住 Dock 标题栏的任何地方（标签或空白处）拖动超过 6 个单位开始拖动，Dock 留在原位，指针下面的窗口里出现停靠指示（和 Visual Studio 一样）：
    区域主体四条边的中点各一个按钮（沿整条边放），指针下的 Stack 中央一个十字（中间叠进去，四周分到那一边）；不允许的方向不显示，
    Toolbar 上只有右边。指针移到某个按钮上时高亮将要占的位置，在按钮上松开才停靠；松开在别处（包括编辑器窗口里不在按钮上的地方）就在那里浮动，
    拖动时不在按钮上会显示浮动窗口将出现的位置。
  - 叠在一起的 Dock 的标签（上面两个角是圆角）沿标签行拖动是调换顺序，和主窗口的标签一样滑到新位置；拖出标题栏（上下超出一个标题栏高）才转为移动 Dock、显示停靠指示。
  - 拖动过程每帧轮询光标位置和左键状态（`PlatformApplication::IsMouseButtonDown`），不依赖被按下元素的事件：指针离开窗口后那些事件就不来了。
    元素在按住期间被释放时，`UIWindowRoot` 仍在松开时归还鼠标捕获。
  - 浮动窗口只放一个 Dock 时没有自己的标题栏：Dock 的标题栏就是窗口的标题栏（`WindowCaption`），由 OS 拖动，所以有 Aero Snap、双击最大化；
    关掉这个 Dock 就关掉窗口。OS 在按住左键时移动它（`DockSpace::OnWindowEvents`：这一帧移动了、大小没变）就算在拖这个 Dock，下面的窗口同样显示停靠指示，
    在按钮上松开就停靠回去。有多个 Stack 时才显示带关闭按钮的标题栏。
  - 标题栏的 ⧉ 把当前 Dock 浮动到原位，✕ 或中键关闭；Window 菜单列出当前上下文能放的 Dock（关掉的从这里重新打开）和 Reset Layout。
  - Dock 之间的缝隙可以拖动调整大小。
  - 浮动窗口只在它的上下文是当前标签时显示。
- **布局保存**：`DockLayoutState`（扁平的节点列表，父节点在前）按布局名存到 `/User/Editor/Layouts/<名字>.mtext`，改动 0.5 秒后写入。
  下次打开同类上下文时恢复；不认识或不属于这个上下文的 Dock 被跳过，Toolbar 不在顶行的布局作废，改用默认布局。
- 从布局里拿掉的节点、Dock 和区域在两帧后才释放，因为同一帧的信号传播里可能还有基于它们的 effect 要运行。

场景上下文（`Editor/SceneEditor`）：`SceneContextType` 打开 Level 和 Prefab；`SceneContext` 带 Toolbar，默认布局是 Outliner | Viewport / Output Log | Details（都是空 Dock）。
在 Asset Browser 能打开 Level 之前，用 File > New Scene（`SceneEditorSubsystem`）打开一个未保存的场景。标签图标暂时借用 `Square`（主题 token `Scene`）。

- `DockSpace::SetToolbar(构建)` 填 Toolbar 的内容（在 Toolbar 的 owner 下构建，替换原来的）。

Asset Browser（`Editor/AssetManager`）：

- **数据**（`AssetCatalogSubsystem`）：用 `AssetIndexClient` 连项目的 Indexer（06 §2，没有就拉起），在本进程读它发布的共享映射（`ReadView`）；
  每 0.5 秒看一次映射的代数（`GetViewGeneration`），变了才重新读：`/Project/Content`（显示为 Content）和 `/Engine/Content`（显示为 Engine）下的每个 `.masset`，
  每个对象是一项（名字、类型、文件、所在目录、文件大小和修改时间、Tags），目录建成树并统计每个目录及以下的资产数。`Version` Signal 在每次重读后变化。
  - 收藏（Starred）按项目存在 `/User/Editor/AssetManager.mtext`（`AssetManagerUserState`）。
  - 每种源类型怎么显示来自 Config `Editor.AssetTypes.Styles`（类型名 → 显示名、主题图标、主题颜色 `AssetType.*`），没列出的显示去掉 `Source` 的类型名和文件图标。
  - Referenced By 用 `AssetIndexClient::GetReferencers`。
- **上下文**（`AssetManagerContext`）：三个 Dock 共享的浏览状态（`AssetBrowserState`：目录或 Starred / Recently Modified、搜索词、类型筛选、排序、
  缩略图或列表、缩略图大小、选中的资产、折叠的目录），以及按它们算出的可见资产（`Computed`：在目录及子目录里、名字或 Tag 含搜索词、类型匹配；按名字、类型或修改时间排序）。
  `DockSpace` 在第一次显示时才建（那时所有 Editor Subsystem 都已初始化），建在上下文自己的 owner 下；布局名 `AssetManager`。
- **Toolbar**：Import、Create、Save All、Refresh、Find Unused、Validate、Source Control；目前只有 Refresh（让 Indexer 重新扫描）可用，其余等 Command（§4.3）。
- **Dock**（都可以关掉、移动、浮动，Window 菜单重新打开）：
  - Folders：FAVORITES（Starred 带数量、Recently Modified 是 7 天内改过的）和 CONTENT 目录树（展开 / 折叠、数量、选中的目录用打开的文件夹图标）。
  - Browser：路径（可点）、搜索框、排序菜单；每种类型一个筛选 Chip 和 Clear filters；缩略图网格（`AutoFillColumns`，类型图标和颜色、收藏星标）或列表
   （名字、类型、路径、大小、修改时间）；底栏是数量和选中、缩略图大小滑条、网格 / 列表切换。单击选中，双击用对应的编辑上下文打开（`OpenAsset`），
    右键菜单：Open、Show in Folder、Copy GUID、Copy Path、Star / Unstar，Rename 和 Delete 暂时不可用。
  - Details：预览（暂时是类型图标，Lit / Unlit / Wire 只是样子）、名字、类型、收藏；Open、Show in Folder、复制 GUID；Properties（类型、源类型、路径、大小、修改时间、GUID）、
    Tags（点击按它搜索）、Referenced By（点击跳到那个资产）。
- 尚未实现：缩略图（GlobalStore 里的）和预览视口、Folders 标题栏的新建目录按钮、目录的拖放和重命名、版本控制状态、状态栏的提示、按资产类型的更多属性（三角形数、作者……）。

- 尚未实现：Dock 的内容（场景的 Dock 还是空的）、视口、场景 Toolbar 的按钮、菜单项接到 Command（§4.3，现在菜单项直接带执行函数）、键盘操作菜单、拖动时按 Esc 取消。

## 6. PIE

**决策**：默认在进程内复制 World，也可以选择拉起独立进程。

- 点击 Play → 把当前 Edit World 快照（序列化到内存中的二进制格式，再反序列化）成一个新的 `Play` World，并创建新的 ScriptDomain、PhysicsScene、RenderScene → 显示在 PIE 视口中。
- 停止时销毁 Play World，Edit World 保持不变。「保留 PIE 中的修改」可以把选中实体的组件值通过 Transaction 写回 Edit World。
- 多人测试：可以在同一个进程里开多个 Play World（一个 ServerWorld 加 N 个 ClientWorld，通过内存回环连接），
  也可以拉起独立的 `Game` / `Server` 进程，通过 Remote 层连接回编辑器（日志、调试、热重载）。
- Simulate：不复制 World，而是把当前 World 的 Profile 切到 `EditorSimulate`（05 §1.1）。不生成玩家，只运行模拟，可以继续编辑和选择实体；停止时切回 `EditorEdit`，并用进入时的快照恢复。
- **编辑模式**（地形雕刻、植被刷、动画录制……）是 `EditorMode` 扩展点：它提供工具面板、视口交互、Command，激活时往 World 上 `PushSets(...)` 叠加自己的 SystemSet，退出时弹出。新增模式不需要改引擎。

## 7. 版本控制

`ISourceControl`（状态、锁定、签出、提交、历史、diff），默认实现是 Lore。Indexer 把文件状态一起索引起来，Asset Browser 直接显示。
二进制文件提供独占锁；MarlinText 文件可以合并，冲突时使用 `MarlinTextMerge`（06 §1.3）。

**决策**：引擎不绑定某一种版本控制。`ISourceControl` 是 Provider（13 §8），放在 Developer 模块里（Indexer 是 Program，也要用；编辑器界面在它之上）。
各实现的 `ShouldCreate` 识别工作区（`.lore`、`.svn` 等），`Engine.SourceControl.Provider` 可以指定。

Indexer 只用只读的 `QueryTracked(目录, visitor)`（06 §2 的提取结果）：每个受控文件给出路径、大小、VCS 名、已提交内容的哈希（不透明字节，最多 32 字节）、
`Clean`（VCS 不读文件内容就确认工作副本等于已提交内容）和版本控制状态。各实现：

- **Lore**：链接 Lore 的 C API（`lore-capi`，`lore.h`，Rust 编出的 `lore.dll` / 静态库，ThirdParty 模块）。`lore_revision_tree_*` 遍历当前 revision 的树，
  得到每个文件的大小和 BLAKE3 哈希（大文件是分块列表的根，仍是确定的）；`lore_repository_status`（不扫描）给出已标记 dirty 的文件，其余为 `Clean`：
  Lore 自己按大小、修改时间维护 dirty 集，必要时才重算哈希。被 Lore filter 改写过的文件（工作副本字节和仓库里不同）不报 `Clean`。
- **SVN**：链接 libsvn_client。`svn_client_info4`（depth infinity）给出每个文件 pristine 的 SHA-1 和 `recorded_size`、`recorded_time`；本地的大小和修改时间
  都等于记录值才是 `Clean`，和 SVN 自己判断修改的方式一样。设了 `svn:eol-style` 或 `svn:keywords` 的文件工作副本字节和 pristine 不同，不报 `Clean`。
- **Perforce**（可选）：`p4 fstat -Ol` 给出有版本的 digest（MD5）和大小；没有打开编辑的文件按 Perforce 的约定视为等于有版本（只读工作区）。
- Git 暂不支持：LFS 指针文件的哈希不是内容哈希，index 的 racy-clean 也要额外处理。
