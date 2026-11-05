# CZ animation 模块

本模块保留 Godot 原有 AnimationTree / AnimationPlayer 图求值，将骨骼轨道直接采样到可复用 SoA 分量，集中拥有批处理实例状态、插值历史、modifier 值快照和 Skin 输出。公共类型为 AnimationBatchProcessor、SkeletonAnimationPose、BoneRotationOffsetModifier3D；完整签名与逐方法说明见 doc_classes。

构建开关为 module_cz_animation_enabled。关闭模块后，这三个类型及模块专属 BlendSpace 扩展均不存在。Game 生产源码/SDK 要求与本模块配对；Game 的 cz.anim.BatchEnabled=false 同步对照仍是含模块的二进制，不是纯官方引擎或可直接编译的 module-off Game。

## 帧与线程合同

所有注册、配置、公开输入变更、提交、等待、发布、统计读取都在主场景线程调用。独立 Pose 在主线程 capture 后，可由调用者独占一个 worker 执行 evaluate；调用者必须 join 后再查询、变更、publish、release。不要同时把同一个可变 Pose 交给独立 evaluator 和 processor。

1. 主线程捕获输入，使用 register_tree 或 register_player 注册有序 Pose。
2. 每个 handle 每次 submit 前只 queue 一次。queue_update 是 sample/display/exact 全真的便捷入口。
3. submit 完成 Prepare，按 parent/child 单元调度 worker；少于16单元或有效worker只有1时同步执行。读取 workers 指标判断真实调度。
4. complete / complete_batch 只等待：不写场景，不发布 method/audio，不发延迟动画信号。
5. publish(handle) 发布单项但不结束整帧；complete_and_publish / finish_all 等待并按依赖顺序发布、收尾。重复发布不重复事件。
6. unregister_animation（或兼容别名 unregister_tree）丢弃待发布输出并恢复旧 callback mode。注册不重启原 Tree/Player 播放状态。

Pose 输出查询等待它的已注册 producer，但不发布事件，也不强制精确重采样。它的 final/display 输出与 Skeleton3D 的 public base 输出是两种语义：发布阶段临时呈现 base→pose_updated、final→skeleton_updated/attachments/skin，最后恢复 public base。base/final 时间历史使用对齐的插值时刻。

pending 帧和发布调用在需要期间持有 processor 强引用；idle controller 不永久自持。注销与销毁不能硬取消正在读取的 worker，必须 join 后清理。RefCounted 在后台 finalizer 中销毁时只排主线程 scene-cleanup payload，SceneTree 帧收尾/显式 drain 再恢复场景所有权。确定性交回控制请在主线程显式注销/release，不依赖 GC 的具体时刻。

捕获通过 ObjectID 识别节点，不接管节点生命周期。回调可以删除/注销/重定向目标；发布在每个 callout 后校验对象身份、capture generation 和输入版本，失效输出会被拒绝或重新求值。资源/场景必要写前 hook 同步读者；外部调用仍应遵守主线程变更合同。

当前共享纯值求解路径覆盖受支持的 BoneRotationOffset、CopyTransform、TwoBoneIK 与零duration LookAt。脚本/自定义重写、时间型 LookAt、show_rest/有效deprecated global override、同骨同channel重复track，以及不支持的离散/脚本属性或嵌套动画组合会被拒绝捕获或同步fallback；这不是把未知行为静默忽略。不兼容 Skeleton observer 会禁止延迟时间插值并强制采样/展示，本身不等于整个native路径fallback。普通图、root motion、blend-shape、method/audio仍按既有Mixer语义执行相应阶段。以实际fallback reason/counters验证具体图是否走native，不把类型注册成功当作全部输入都受支持。

可变 Pose/Skeleton 不能属于两个独立注册。跨 Pose 的 copy/attachment 来源必须无环，且是同一注册内更早的 Pose 或显式 parent 单元。未表达顺序的平行 entry 不允许互读。

## 精确采样、时间 LOD 与骨骼 LOD

queue_update_with_options(handle, delta, sample, display, exact, time, interval, force_full_bones=false) 的 delta 始终推进图；sample=false 只请求跳过骨骼采样。time 是调用者单调 presentation 秒数，interval 是采样间隔/展示延迟。两次有效兼容历史可在延迟时刻插值；缺历史、输入变更、不兼容 observer 或 interval=0 可强制采样。

exact=true 请求 sample/display 并丢弃本次展示延迟，和完整骨骼是两个独立维度。force_full_bones=true 临时使用完整拓扑，不改 configure_bone_lod / set_bone_lod 的配置级别，也不改 Game distance/time LOD。

configure_bone_lod 接收显式 bone_names 与 first_disabled_lod。数组等长、名称非空且唯一、首次禁用级别>=1；LOD0完整。缺名在捕获时失败，不模糊匹配、不启动加载模型猜组。空配置始终完整。祖先与 modifier/socket/copy/attachment 所需骨保留；compact 求值后用 inactive rest 展开到原始完整 Skin palette。Game 的 CZBoneLodProfile 是独立纯配置 Resource，不能把 body profile 套给不同骨架的 head/hair。

set_bone_lod_enabled 与 set_pose_simd_enabled 改全局设置前完成已有帧，同值快速返回。SIMD enabled 只表示请求，需 pose_simd_available 判断当前构建/平台内核是否可用；double 等不支持路径回退 scalar。两条路径保持 Godot Quaternion.slerp、rest 顺序和缺轨语义，不用 nlerp 代替。

## API 迁移与 BlendSpace 扩展

| 旧入口/行为 | 当前入口 |
| --- | --- |
| AnimationTree.SetParameterGroup | AnimationBatchProcessor.set_parameter_group(tree, StringName[], value)，同一个值写入全部路径 |
| RenderingServer 自定义 skeleton buffer API | AnimationBatchProcessor.set_skeleton_buffer / get_skeleton_buffer_statistics；渲染内部 MeshStorage 接口，不扩 RenderingServer 公共API |
| 旧核心内的 AnimationBatchProcessor / SkeletonAnimationPose / BoneRotationOffsetModifier3D | 相同公开类型名，由本模块注册 |
| BlendSpace 共享 Resource 内批处理历史 | module bridge 按 Tree + node path 持有运行态 |

set_parameter_group 先检查所有路径与兼容类型，再按转换后的 Variant 写入；失败不部分更新。live Tree 的只读参数不可写。旧 Tree 同名方法不恢复。

模块启用时 AnimationNodeBlendSpace1D/2D 多出 Resource 成员 sample_weight_interpolation_speed（默认0，负值截到0），通过同名 get/set accessor。它按 speed*delta move_toward 各 sample 权重后归一化；0立即使用几何权重。seek / sample count 变化重置历史；delta=0保留已有历史。历史按 Tree/path 隔离，不改变 blend_position 输入。

每 Tree 的动态参数为 cycle_duration_override（float，可写，默认0）和 effective_cycle_duration（float，只读，默认0）。例如 parameters/Walk/cycle_duration_override；若 BlendSpace 是 Tree 根，省略 Walk。它们不是 Resource 的导出 member。cyclic 模式下，effective 值为有效正长度的当前权重均值，独立于 override；非cyclic计算重置为0。override 大于引擎比较epsilon才替代该次共享周期；否则保留对应 mutable/fixed timing。不修改共享 Resource 的 sync_mode / cyclic_length。

## Skin 上传和真实统计

set_skeleton_buffer 的 buffer 是每个3D bind 12个 float32：三行3×4，含 translation。RID 必须已按匹配骨数分配。call_on_render_thread 的 Callable 保留 COW payload 直至消费，调用返回不代表渲染队列或 GPU 完成。

get_skeleton_buffer_statistics(reset=false) 默认只读已被 backend 消费的原子计数；reset=true 原子消费。RD 输出 apply_calls/bytes、upload_passes、dirty_skeletons、upload_calls/bytes、skin_dispatches/vertices、reshape_dispatches。不支持统计的后端（包括当前 GLES 默认实现）返回空 Dictionary，不能将缺失指标伪造为0。比较真实上传时需另控制 frame_post_draw / queue sync；这仍不是 GPU completion 计时。

get_statistics 不自动 Complete。frame 为唯一提交号；fallbacks/failed/recomputed/publish_entries 用于检验实际结果。pawns/animals/weapons 分别代表 tree / 无parent player / 有parent player 的注册分类。samples/displays 表示该帧有效请求计数，不能代替真正 sampler 计时证据。

method_validation_cache_hits / method_validation_scans 记录方法名安全证书的命中与重扫；method_validation_animations / tracks / keys 记录实际扫描量（含遇到不安全方法提前退出的部分扫描）。这些计数每次submit清零，并累加发布重算期间的验证。证书按注册的固定safe_methods、track_map_version、animation_set_update_pass及清缓存失效；仍检查未播放的全部clip，图节点/script/observer/transition合法性保持动态校验。证书只覆盖method名称权限，不声明共享args数组不可变。动画库与Mixer库入口在主线程写前等待，独立worker构造新资源不会自等待。

Prepare/Wait/Publish 是主线程 elapsed μs；SampleBlend/Interpolate/Matrix/SkinPack 是各 Pose 的 elapsed work sum，可以在 worker 上重叠，Matrix 也可能含主线程 capture；不能加总成帧 wall time 或 CPU core time。worker尚未join时 Pose 阶段字段不提供。captured_bones 来自实际 native 分量重抓；稳定动画帧应为0。pose_memory_bytes 是模块记录的 Pose/SoA/骨/skin容量子集，不是 total animation/process resident memory。skin_buffer_copies 是实际 COW pointer replacement 累积次数。

### 可选原生阶段诊断

`AnimationBatchProcessor` 构造时只读取一次环境变量 `CZ_ANIM_PHASE_DIAGNOSTICS`，仅值为 `1` 时开启；构造后的环境变化不会切换既有实例。默认关闭，不执行新增阶段时钟读取。`get_statistics()` 始终提供 bool `phase_diagnostics_enabled`；关闭时下列新增耗时/次数键缺失，不伪造为实测0。没有新增ClassDB或托管ABI入口。

开启后，主线程在当前提交已完成发布（frame>0、!pending、!preparing、!publishing、group==-1）时提供：

|键|单位和范围|
|---|---|
|submit_total_us|通过guard后的C++ Submit主线程wall，含准备/unit组织/inline执行或任务提交，不含C#封送和调用方工作|
|submit_inline_us|Submit内整个inline batch循环；实际走异步提交时为0|
|evaluate_total_us|当前提交各entry的完整Bridge.evaluate elapsed work总和，含TLS scope收尾和Player状态保存/恢复|
|evaluate_graph_us|常规帧的_blend_init、_blend_pre_process与processed时的_blend_calc_total_weight|
|evaluate_weights_us|常规实例容量、owned raw/normalized权重快照或eventless借用视图的零值判据与元数据准备|
|evaluate_pose_us|完整Pose循环，包含采样、modifier/pawn求解、历史、显示展开及pack等|
|evaluate_aux_blend_us|evaluate侧官方_blend_process，含track公共前缀/skip及root-motion/BlendShape/value等；不含publish事件侧第二遍|
|evaluate_calls / evaluate_first_key_calls|当前提交的evaluate调用次数 / Player started零delta特殊分支次数|

两个Submit计时与每个State累计在每次有效Submit重置，State不在每次evaluate或prepare_frame重置。dirty输出在Publish重算时继续累计evaluate计时/次数，并由既有recomputed标记；这部分不属于Submit。Player首键零delta分支及其快照属于evaluate_total的未单列部分，不混入常规graph/weights阶段。

graph/weights/pose/aux是evaluate_total的互斥子区间；旧SampleBlend/Interpolate/Matrix/SkinPack是其部分子工作，Matrix还可能来自prepare/capture，不能重复相加。四人inline、recomputed=0、无fallback/failed、evaluate_calls符合预期且first_key_calls=0时，才可分别用 `submit_total - prepare - submit_inline`、`submit_inline - evaluate_total` 和 `evaluate_total - graph - weights - pose - aux` 分析未单列开销。多worker的evaluate值是可重叠work sum，不能反减主线程Submit wall或称为CPU-core time。负残差须保留排查，不归零掩盖边界错误。

这是独立诊断模式，有新增时钟观测成本；正式timing-only/P95应关闭它。GetStatistics不隐式等待/发布，未到上述完成点时新增阶段键缺失。对应native回归已随当前016统一构建通过，覆盖开关锁定/关闭缺失、完成后可见、逐Submit重置、真实Player dirty重算、空提交及worker汇总。

## 配置字典与空间

SkeletonAnimationPose 的 configure_aim / configure_ik 是兼容现有 Game pure-value solver 的字典接口，所有 *_bone 键是原始骨架0基索引，默认-1；配置实际被使用的链时必须给合法索引。这些配置骨会加入 LOD required closure。world位置与normal使用调用者与 Skeleton3D 一致的世界单位；hand_to_* 是手骨相对量；ObjectID键用非负整数，0不发布对应节点目标。

configure_aim 的实际键由当前实现读取：aim_bone、hand_bone、hips_bone、chest_bone、upper_chest_bone、left_leg_bone、right_leg_bone、left_arm_bone、left_elbow_bone、left_hand_bone；left_arm_rest_pole（默认(0,0,1)）、hip_yaw_weight（0）、hand_to_muzzle（identity Transform3D）、可选 hand_to_grip、可选 hand_to_elbow_pole、target_world（zero）、up_world（UP）、grip_target_id / elbow_pole_id（0）。modifier_id 关联主线程 modifier 完成回调。空的可选grip/pole键与键不存在含义不同，按方法XML说明设置。

configure_ik 空字典禁用求解。实际键包括 has_hand_target、hand_target_world、left_hand_weight、left_foot_weight、right_foot_weight；hips_bone；left_upper_arm_bone、left_lower_arm_bone、left_hand_bone、left_arm_rest_pole；left_upper_leg_bone、left_lower_leg_bone、left_foot_bone、left_toes_bone、left_leg_rest_pole、left_leg_length；对应 right_* leg/foot/toes/rest_pole/length。其余标量键为 leg_reach_epsilon、maximum_ground_slope_degrees、sole_offset_cm、ground_motion_contact、foot_source_stance_calibration_cm、maximum_pelvis_drop_cm、maximum_pelvis_raise_cm。

每侧地面键为 left_ground_has_hit、left_ground_position、left_ground_normal、left_ground_smoothed_normal，以及对应 right_*；smoothed未给时使用raw normal。目标ID键为 hand_target_id、elbow_pole_id、pelvis_target_id、left_foot_target_id、left_knee_pole_id、right_foot_target_id、right_knee_pole_id。bool默认false、标量/位置/pole默认0、Transform3D默认identity。动态set_ik_hand_input.weights=(左手,左脚,右脚)。

## 验证边界

当前Windows native016统一筛选为104/104用例、106188/106188断言；double及Tracy各104/106188，CZ全关闭21/210，原声明LookAt missing_target warning保留。两项#if0旧内部谓词档案不计入运行数。对应冻结为native-frame-eventless-freeze-016.json；本次后续仅文档整理，不重编二进制。Game/图形/平台和性能证据仍分别列范围，原生通过不等于完整计划验收。macOS ARM64/NEON尚未实机运行。

匹配45-callback SDK配套的Game015已执行1327项普通测试（1320通过、7个历史失败方法保留）及156项Godot逻辑测试。两条声明native ERROR使严格适配器仍返回false，原始结果保留；没有新增错误。真实CLR退出pin/join/drain有旧负对照1次和新正对照3次，不能据此宣称原稀发交错已精确复现或所有用户后台线程均受治理。

### Pose fastpaths and controlled comparison

Pose fastpaths are enabled by default. `CZ_ANIM_DISABLE_POSE_FASTPATHS=1`, read once when a processor is constructed, selects the previous sampling/weight-copy/track-prefix flow for a controlled comparison. This internal diagnostic switch does not revert correctness fixes, change graph time, events, SIMD selection, LOD, or worker scheduling. It does not add a ClassDB or managed API.

For deterministic mixing, the sampler reads each instance's raw weights directly. Event-bearing/Player paths own a snapshot through event publication; the strict eventless Tree path described below borrows only during Evaluate. Non-deterministic mixing still uses owned normalized weights. A sampled Pose/instance initializes its scratch pose and channel weights only when the first nonzero effective channel successfully interpolates. If no channel writes, it leaves the blended pose unchanged. Scratch contents are private and unspecified for such a call. Track order, the original near-zero test, and ordered quaternion interpolation remain unchanged; an instance is never skipped solely because its scalar weight is zero.

Within module evaluation/publication, early track skipping retains enabled/cache/index checks and the original non-deterministic zero-total-weight behavior. It updates root-motion classification before skipping a track, then computes weights only for surviving tracks. The official synchronous and module-disabled flows retain their original order. Root-motion path changes synchronize pending work and invalidate binding/reference/history caches; changing the local-root-motion option also synchronizes pending work.

With `CZ_ANIM_PHASE_DIAGNOSTICS=1`, completed statistics additionally report actual animation instances, mixer track-count sums, raw weight elements (including Player first-key snapshots), normalized-copy elements, and sampler calls/zero-weight/no-written/empty-binding calls. These are counts, not memory-bus traffic or timing. They reset per Submit, include dirty publication re-evaluation, and are absent before completed publication or with diagnostics disabled. Existing phase timers include their diagnostic overhead; do not use an enabled diagnostic run as formal whole-frame acceptance evidence.

Only deterministic fastpaths can additionally certify an instance's exact +/-0 weights for the union of all registered Poses' actual bone blend indices while copying its owned snapshot or inspecting its current borrowed view. Non-finite instance weights, nonzero/non-finite masked elements, invalid indices or an unprepared mask use normal sampling; non-deterministic and disabled-fastpaths flows do not use this certificate. Masks are built during existing binding reconstruction and unioned only after a binding change, never by a steady-frame animation-library rescan. First-key and event-bearing instances remain owned; auxiliary/root-motion work and per-Pose evaluation remain intact.

Completed phase diagnostics additionally expose `sample_certified_zero_weight_calls` (a subset of existing zero-weight/no-written calls, retaining the old call/empty-binding counts) and `zero_weight_mask_rebuilds` (actual union reconstructions, normally zero on unchanged warmed frames). These counts are not a performance promise. The behavior regressions passed in the matching native016 unified build/run.

### 空闲同步入口

没有 pending work 或仍在 Prepare 时，mixer 同步入口在 owner mutex 内直接识别既有空操作；没有 worker group 的 Pose 查询同样不再临时增加 Processor 引用。真正有待处理工作时仍保留 owner Ref pin，pending Tree 仍发布，Pose 查询仅等待 worker、不发布事件。此改动不改变 Player publication 回调中的 dirty 标记，也不改任务、采样或 API。native 回归通过公开 instance-binding reference callback 校准并观测空闲 1→2→1 引用边沿，另检查 pending 查询/发布行为；它不是托管 GCHandle 分配或性能实测证据。

### 纯骨骼实例的辅助轨道遍历

deterministic mixing、同 mixer TLS 且构造时 fastpaths 开启时，可进一步省略 `_blend_process` 中已经没有剩余工作的单个动画实例轨道循环。证书仅接收至少一个 enabled 轨道，且所有 enabled 轨道都有有效 cache/blend index、属于已注册 Skeleton 的非 root-motion TRS 通道。其 cache path 必须与轨道一致，`root_motion` 标志已经为 false，使原逐轨赋值本来就是无变化操作。禁用轨道不参与资格判定。root-motion、method/audio、BlendShape、普通 Node3D 属性、无效映射及非 deterministic mixing 保留原循环；首版 Evaluate 和 Publish 使用相同的严格证书。

证书附着于模块的每个 Pose binding，并在现有绑定重建扫描中生成；所有已注册 Pose 的交集随原零权重 mask 重建计算。没有新增稳定帧全库扫描。读取时核对 library/binding 版本与 Pose layout/dirty 状态；缓存失效会清空证书。原动画 cache 查找及缺失错误检查仍在 shortcut 之前。此 shortcut 不省略图求值、总权重、raw/first-key snapshot、SoA 采样、事件发布顺序或官方 apply/post-process，也不按实例权重为零跳过音频。

阶段诊断开启且整批完成发布后，`evaluate_instance_track_skips` / `publish_instance_track_skips` 记录本帧实际省略的辅助轨道循环实例数，包含既有重算；Publish 值直接读取发布结束后的 State。Complete 尚未发布、回调重入中或诊断关闭时不输出这些字段。它们不是省略的 SoA 实例数、轨道数、CPU 时间或性能收益。首键/开关、事件和cache替换、root-motion切换及多Pose/LOD行为断言已随native016通过。

### 本帧实例无事件 Tree 的 Evaluate 内权重视图

已通过现有脚本/图节点/observer/表达式合法性校验的 native AnimationTree，在 deterministic mixing 且 fastpaths 开启，并且本次 Graph 产生的所有 AnimationInstance 的动画都没有 enabled METHOD/AUDIO 轨时，可以只在 Evaluate 内借用该 Tree 自己的 AnimationNodeInstance 权重。同一轮全库扫描缓存每个 Animation 的 eventless trait，方法名安全检查仍覆盖全部库；未参与本帧的事件 clip 不封死借用。Graph 成功后检查全部实际实例，包括零权重实例，任一事件或未知 trait 都使整帧 owned；零 key 事件轨和 safe method 名不能使实际事件实例变为无事件。Player/首键、非 deterministic、fastpaths 关闭、证书或图状态不确定时保留原 owned snapshot。逐动画 trait 顺带现有全库验证扫描生成，按 cache validity、track_map_version、animation_set_update_pass 与清缓存失效，稳定帧不新增整库扫描。

借用分支不构造持久 AnimationInstance/Animation Ref 副本，也不保留图缓冲裸指针到 Publish；Pose 和辅助 root-motion/BlendShape/value 消费当前 official instances，多个 Pose 复用每实例的 owned 零值 bool。Evaluate 的 scope 收尾在所有返回路径清空这些 official instances。Publish 锁存本次求值模式，不重建事件实例列表；仍调用空列表的官方 _blend_process 保留固定 bookkeeping，再执行原 Pose.publish、apply、post、mixer_applied 和已排队图信号。其 `publish_instance_track_skips` 因实际没有实例遍历而为0，不能伪造为旧逻辑实例数。

有写入时沿用写前发布/等待或 dirty entry 的 discard→prepare→evaluate。借用帧在 Publish 前发现 cache/set/map/root/process-pass 或图状态不符会失败，不会回落读取不存在的 owned snapshot，也不会把当前图的新权重冒充旧帧。任意未同步的外部线程资源改写不在既有工作线程合同内。Prepare 仍逐帧保留两遍动态安全检查；BlendSpace 历史 map 只预建实际1D/2D节点的条目，不再为其它节点执行无用 has/insert，这不是拓扑缓存。

诊断开启且发布完成后，`evaluate_raw_weight_elements` 保留历史**逻辑 extent**（每实例 track_count，含 Player 首键），采用借用后不能再称实际 copies。新增 `evaluate_owned_weight_copy_elements` 是 owned raw 数组实际写入数（含默认1）；`evaluate_borrowed_weight_elements` 是按实例累计的实际可见源向量范围，别名可重复计入，非唯一存储/读取/复制数；null源计0，缺失元素的默认1不算源元素。`evaluate_zero_weight_elements_inspected` 计 exact-zero 谓词实际比较的 masked 元素（含隐含默认1）。`evaluate_borrowed_weight_calls` / `publish_eventless_calls` 分别计已处理的借用 Evaluate 次数及真正无快照 Publish 次数；dirty 重算可使它们不同于唯一 frame 数。另有 evaluate_eventful_instances / evaluate_unknown_event_traits，在候选 Tree 的实际实例判定中计事件/未知 trait 实例（含零权重）；诊断开启时扫描完整实例列表计数，关闭时遇首个拒绝即短路，不增加时钟或整库扫描。所有计数沿原诊断开关/完成点/逐Submit重置合同，不新增 ClassDB/托管ABI入口。

新增行为回归覆盖auxiliary/root-motion与原始同步/owned对照、unused method/audio/零key/库与enabled失效、实际Walk↔Work切换与仅Publish触发cue、零权重事件实例否决、实际音频主线程发布、dirty重算、发布回调换图、写前同步、16单位worker、首键owned与多Pose/LOD，另检查不一致的外部直接Advance安全失败；均随native016统一构建通过。

2026-11-05按用户时间要求收尾：当前专用crowd整帧54组有效，64/256所有模式三轮通过原5%门槛；4人分段LOD有两项超限（LOD1 r1=1.137456、LOD3 r1=1.053980）。详细CPU阶段仅完成11/45，后续和预注册同窗口4人18组取消，不能宣布完整性能验收。历史模型/插值/深度/空PhysicalBone数据按其原冻结版本保留。完整交接在Game/docs/WIP，不发布主Editor或改共享资产。
