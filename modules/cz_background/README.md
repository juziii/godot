# cz_background

本模块集中维护 ColonyZero 后台验证使用的操作系统策略，使用 Godot 标准 module_cz_background_enabled=yes/no 开关。未编译模块时，所有官方接入位置保留官方行为。

CZBackground::is_background_mode() 只检查启动环境中是否存在 GODOT_BACKGROUND_MODE，不解析布尔值，且只缓存首次结果。macOS 的 OS 构造、AppKit 激活、显示、最小化、全屏和缩放入口共同使用它；后台模式不会激活应用或主动显示窗口。该策略不保证被完全遮挡的 Metal 窗口继续产出 drawable，需要 macOS 实机验证。

Windows 的 is_current_thread_input_desktop() 每次查询当前线程 desktop 与当前 input desktop，比较系统返回的名称。查询失败时拒绝系统光标和输入操作。它不缓存结果，因为 input desktop 可在进程运行时切换。is_current_process_foreground() 仅判断当前前台窗口的进程归属。helper 不依赖模块注册、ProjectSettings 或引擎单例，可用于启动早期。

官方接入保留在 platform/windows/display_server_windows.cpp、四个 macOS 原生文件以及 editor/run/{embedded_process,game_view_plugin}.cpp。窗口和嵌入对象继续持有自身状态；模块不缓存 HWND、Control、Window 或进程对象。PIE 浮动窗口按原生装饰区约束位置，设置变化不会恢复最小化窗口。延后的焦点请求在真正执行时重新核实宿主/游戏的前台关系。

测试：tests=yes module_cz_background_enabled=yes 构建后使用 --test --test-case="*[CZBackground]*"。Windows 回归将新 worker 绑定到私有 desktop，验证其不能被当作 input desktop；不切换用户 desktop，不移动光标，不激活窗口。macOS 焦点、Metal 后台渲染及真实 Editor/PIE 行为仍需目标平台集成验收。
