# 15 · 输入

游戏层的输入：把 `Application` 的原始事件和手柄状态（12 §1.3）变成玩法读的**动作**（action）。窗口、键位、IME 这些平台层的东西不在这里。

## 1. 分层

```
Application（原始事件、手柄状态）
   │  每帧 BeginFrame
InputDevice（Provider）── 写 → 控件（Control）的值："Keyboard.W"、"Mouse.Delta"、"Gamepad.LeftStick"
   │
InputContext 栈（资产，按优先级）── 绑定 → InputAction（资产）的值
   │  累计计数
每个 World 的 Resource<InputActions>，在 Fixed 和 Variable 两个 Stage 开头各刷新一次 → System / 脚本读取
```

- **决策**：输入的**定义**是资产，**全局设置和玩家改键**是 Config。
  - 动作和绑定由内容引用（组件、Prefab、脚本按 UUID 引用，改名不断），按功能各自交付（引擎的调试相机、项目的步行 / 驾驶 / 菜单），
    走资产管线的依赖追踪、打包裁剪和热重载，将来的绑定编辑器只是普通的资产编辑上下文。
  - 死区、灵敏度、反转、改键是玩家可写、按平台分层的数据，正好是 Config 的分层（06 §1.4）。
- 两个模块：`Runtime/Input`（资产、控件、`InputDevice` 扩展点、`InputSubsystem`、System；依赖 Asset、Config、Engine、World，**不**依赖 Application，
  所以 `MarlinConverter` 链接它就认识这两种资产）和 `Runtime/ApplicationInput`（键盘、鼠标、手柄三个设备，依赖 Application）。Game 两个都链接。
  Server 不链接 Application，也就没有设备；网络上传的是动作值，不是原始输入（以后的预测和回滚在 Net 里做）。

## 2. 控件与设备

- **控件**是一个名字（`Name`，`<设备>.<控件>`）加类型：`Button`、`Axis1D`、`Axis2D`，以及它是**状态**（按住多少）还是**量**（这一帧动了多少：鼠标位移、滚轮）。
- **决策**：设备是 Provider 扩展点 `MCLASS(Abstract, ExtensionPoint) InputDevice`（13 §3），启动时 `RegisterControls` 登记自己的控件，
  每帧 `Update` 从事件和平台状态写控件的值。方向盘、飞行摇杆、VR 控制器、主机专属手柄都是新的设备，不改 Input 本身。
- 引擎自带：
  - `KeyboardDevice`：`Keyboard.<KeyCode>`，名字取自 `MENUM KeyCode`（按物理键位，美式布局命名），如 `Keyboard.W`、`Keyboard.LeftShift`。
  - `MouseDevice`：`Mouse.Left` / `Right` / `Middle` / `X1` / `X2`；`Mouse.Delta`（Axis2D 量，raw 位移，乘 `MouseSensitivity`，右和下为正，`InvertMouseY` 时取反）；
    `Mouse.Wheel`、`Mouse.WheelX`（Axis1D 量，单位是格）。
  - `GamepadDevice`：第一个连接的手柄；`Gamepad.<GamepadButtons>`（`South`、`LeftShoulder`、按下摇杆是 `LeftStickPress`…），`Gamepad.LeftStick` / `RightStick`（Axis2D，上为正，径向死区），
    `Gamepad.LeftTrigger` / `RightTrigger`（Axis1D，死区）。
- 主窗口失去焦点时所有设备回到静止，按住的键不会卡住。

## 3. 资产

```
InputAction { !UUID = "…" !Name = "FlyCameraMove"  ValueType = Axis3D }
InputAction { !UUID = "…" !Name = "FlyCameraLook"  ValueType = Axis2D  Accumulate = true }

InputContext
{
    !UUID = "…" !Name = "FlyCamera"
    Priority = 0
    Bindings = [
        { Action = <FlyCameraMove#…>  Control = "Keyboard.W"  Chord = ["Mouse.Right"]  XTo = Z  Scale = (1, 1, -1)  Name = "Forward" }
        { Action = <FlyCameraMove#…>  Control = "Gamepad.LeftStick"  YTo = Z  Scale = (1, 1, -1) }
        { Action = <FlyCameraLook#…>  Control = "Mouse.Delta"  Chord = ["Mouse.Right"]  Scale = (-0.0025, -0.0025, 1) }
        { Action = <FlyCameraLook#…>  Control = "Gamepad.RightStick"  Scale = (-2.5, 2.5, 1) }
    ]
}
```

- `InputAction`（`MCLASS(SourceIsRuntime)`，04 §4）：`ValueType` 是 `Button` / `Axis1D` / `Axis2D` / `Axis3D`，值统一存成 `Vector3`。
  `Accumulate` 的动作是**量**：鼠标位移原样累加，状态控件（摇杆）乘帧间隔再累加，所以同一个 Look 动作里鼠标是「弧度 / 计数」、摇杆是「弧度 / 秒」，读的一方只看到「这一步转了多少」。
- `InputContext`（`MCLASS(SourceIsRuntime)`）：`Priority` 和一组绑定。绑定字段：
  - `Action`（强引用，context 加载时动作一起加载）、`Control`（控件名）。
  - `Chord`：同时必须按住的控件（`Mouse.Right`、`Keyboard.LeftControl`），没满足时这条绑定不产生值。
  - `XTo` / `YTo`：控件的 X、Y 分量写到动作的哪个分量（`X` / `Y` / `Z` / `None`，默认 X→X、Y→Y），然后按分量乘 `Scale`（负数即取反）。
  - `Name`：非空的绑定可以被玩家改键（§5）。
- 同一动作的多条绑定相加（Button 取最大）。值的大小超过 0.5 算按下（`Down`）。
- 运行时检查：控件名不存在、`Chord` 里有不存在的控件时警告一次并忽略那条绑定。（以后可以换成真正的转换器在转换时报错。）

## 4. 运行时

- **`InputSubsystem`**（EngineSubsystem，`Config = "Engine.Input"`，`Dependencies = "AssetSubsystem"`，`OptionalDependencies = "ApplicationSubsystem"`，01 §3）每帧 `BeginFrame`：
  1. 生效上一帧排队的 context 增删和指针锁定请求（这些 API 任何线程都能调，主线程上统一执行，因为平台要求）。
  2. 设备更新控件。
  3. 按优先级从高到低求值活动的 context：一条绑定产生了值，它的控件对更低优先级的 context 就**被消耗**了，
     所以菜单压在玩法上面时，同一个键不会两边都触发。
  4. 每个动作更新**只增的计数**：按下次数、松开次数、累计量（`float64`），以及当前值。
- **`PushContext(context)` / `PopContext(context)`**：context 栈是全局的（本地一个玩家）；同一 context 压两次要弹两次。
- **指针锁定**：`SetPointerLock(owner, bool)`，有任何 owner 要求时隐藏并锁住光标、鼠标只产生 `Mouse.Delta`。
- **Fixed 和 Variable**：每个 World 有一个 `Resource<InputActions>`，由两个 System 刷新：`InputPhase`（Fixed，每个固定步开头）里的 `InputFixedStepSystem`，
  和新的 `InputFramePhase`（Variable，在 `CameraPhase` 之前）里的 `InputFrameSystem`。每个 System 记着自己上次看到的计数，用差值算出
  `Pressed` / `Released`（这一步里有没有发生）和 `Accumulate` 动作的量。所以按键一定落在某一个固定步里，不会因为这一帧跑了 0 步而丢掉，
  也不会因为跑了 2 步而触发两次；鼠标位移也不会被两步重复计算。两个 System 属于 `CoreSimulationSet`。
- 读法：`ReadResource<InputActions>()`，然后 `actions.Get(action)` 得到 `{ Value, Down, Pressed, Released }`；没有活动 context 绑定的动作是全零。
- 引用动作的组件（如 `FlyCameraInput`）用 Shared：同一套输入的实体共用一份值，System 按 chunk 用 `chunk.Get<T>()` 读，
  不能写成 `EntityQuery` 的 `Read<T>` 项（Shared 组件没有逐行的列）。
- 测试：`InputSubsystem::ApplyRequests` / `Evaluate` 把 `BeginFrame` 拆开，测试自己设置控件值（`Input/Tests` 里的 `TestInputDevice`）。

## 5. Config

- `Engine.Input`（`UserConfigurable`）：`MouseSensitivity`（默认 1）、`InvertMouseY`、`StickDeadZone`（0.2，径向）、`TriggerDeadZone`（0.05），
  以及 `Rebinds`：
  ```
  Input
  {
      Rebinds = [ { Context = <FlyCamera#…>  Binding = "Forward"  Control = "Keyboard.Up" } ]
  }
  ```
  改键按 `(Context, Binding 名)` 替换那条绑定的 `Control`；玩家的改键写在 `/User/Config/Engine.mtext`，平台层可以给主机换默认键。
- 运行时 `InputSubsystem::SetRebind` / `ClearRebind` 改内存里的改键，`Config::Reload` 后重新读取。

## 6. 尚未做

- 脚本访问：需要 ScriptVM 的原生函数注册扩展点（现在 `ScriptDomain` 只注册 Core 和 World 两组），之后 `Input.mscript` 声明 `InputActions.Get(action)`，
  动作在脚本里同样按 `AssetRef<InputAction>` 引用。游戏不写 C++（README 不变量 5），所以这是 Input 的下一步。
- 改键写回 `/User/Config`（Config 还没有写接口）、本地多人（设备分配给玩家，每个玩家一个 context 栈）、
  Tap / Hold 这类触发器、可扩展的修饰器、DebugUI 占用键盘时屏蔽游戏输入、编辑器视口的输入路由。
