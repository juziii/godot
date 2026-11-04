# cz_tracy

本模块复用 Godot 官方 Tracy 实现，提供 ColonyZero C# profiler 使用的可选 C ABI。使用标准 module_cz_tracy_enabled=yes/no 开关；只有 GODOT_USE_TRACY 构建才输出 ABI，普通及其他 profiler 构建不输出这些符号。

ABI 版本为 1，保留现有四个符号：godot_csharp_tracy_bridge_version、godot_csharp_tracy_source_location、godot_csharp_tracy_zone_begin、godot_csharp_tracy_zone_end。调用者由主可执行文件查找导出，缺少 ABI 时继续使用现有 no-op 路径。SourceLocation 复用官方 tracy::intern_source_location 的存储和生命周期；调用应发生在 profiler 初始化后、清理前，zone 必须在开始它的线程中结束。

ABI 和模块注册函数放在同一编译单元，确保静态模块库链接注册函数时也包含导出符号。实现不改写 core/profiling/profiling.cpp，也不注册项目类。源位置缓存位于官方 profiler；Begin/End 只转交 Tracy，不创建 Godot 对象或每帧分配内存。token 使用 memcpy 封装 TracyCZoneCtx，并检查尺寸，避免违反别名规则。

需验证：Windows x64 和 macOS ARM64 的 Tracy 开/关构建、可执行文件导出表、游戏 CZTracyBridge.isAvailable 与一次真实 trace 捕获。此模块本身不增加模拟 Tracy 实现的单元测试；实际采集是 ABI 与链接是否有效的验收。
