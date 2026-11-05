# CZ animation 模块

本模块保留 Godot 原有 AnimationTree / AnimationPlayer 图求值，将骨骼轨道直接采样到可复用 SoA 分量，集中拥有批处理实例状态、插值历史、modifier 值快照和 Skin 输出。公共类型为 AnimationBatchProcessor、SkeletonAnimationPose、BoneRotationOffsetModifier3D；完整签名与逐方法说明见 doc_classes。

构建开关为 module_cz_animation_enabled。关闭模块后，这三个类型及模块专属 BlendSpace 扩展均不存在。Game 的候选模块路径要求配套SDK；旧C基线另用已识别API的编译期兼容分支。Game 的 cz.anim.BatchEnabled=false 同步对照仍是含模块的二进制，不是纯官方引擎或可直接编译的 module-off Game。

## 当前比较开关

下表是本模块源码的默认值，不是性能验收结论。除两个Processor构造时开关外，环境变量均在进程首次读取时固定；修改后应另起进程，不能把同一进程中改环境变量当作A/B。未设置或值不为精确字符串 `1` 时按默认路径执行。

| 环境变量 | 默认路径 | 值为1时的路径 | 实际统计键 |
| --- | --- | --- | --- |
| `CZ_ANIM_POSE_AOS` | 常驻SoA | 常驻AoS/标量参考，不做逐帧布局转换 | `pose_layout_soa`、`pose_simd_effective` |
| `CZ_ANIM_SHARED_TRACK_DATA` | OFF | F05同时共享静态轨道和CZBoneLod描述 | `shared_track_data_enabled` |
| `CZ_ANIM_REUSE_SAMPLE_SCRATCH` | OFF | F06每Pose独占三组采样权重scratch | `sample_scratch_reuse_enabled` |
| `CZ_ANIM_IN_PLACE_FIRST_BLEND` | OFF | F07首份实际blend原地计算 | `in_place_first_blend_enabled` |
| `CZ_ANIM_DISABLE_ZERO_POSE_SKIP` | ON，保持既有零贡献处理 | 关闭F08零贡献/lazy/零权重证书 | `zero_pose_skip_enabled` |
| `CZ_ANIM_SKIN_UPLOAD_PER_BONE` | F12整段palette写入 | 渲染线程逐骨调用官方setter | `bulk_skin_upload_enabled` |
| `CZ_ANIM_DISABLE_NO_MATERIAL_DEPTH_PREPASS` | F13对显式声明的材质启用预通道退出 | 保留其原材质预通道shader | `no_material_depth_prepass_enabled` |
| `CZ_ANIM_DISABLE_POSE_FASTPATHS` | ON | 关闭既有pose fastpaths组合；每Processor构造时读取 | `pose_fastpaths_enabled` |
| `CZ_ANIM_PHASE_DIAGNOSTICS` | OFF | 开启详细阶段诊断；每Processor构造时读取 | `phase_diagnostics_enabled` |

F08 sampler的有效条件是 `pose_fastpaths_enabled && zero_pose_skip_enabled`；后者为true不表示前者关闭时仍使用零贡献优化。SIMD和骨骼LOD另由 `set_pose_simd_enabled` / `set_bone_lod_enabled` 控制，模块全局初值均为true，Game可显式覆盖。SIMD仍受布局、ISA和float/double构建约束；LOD0或没有有效简化配置时不减少活动骨骼。

## 帧与线程合同

所有注册、配置、公开输入变更、提交、等待、发布、统计读取都在主场景线程调用。独立 Pose 在主线程 capture 后，可由调用者独占一个 worker 执行 evaluate；调用者必须 join 后再查询、变更、publish、release。不要同时把同一个可变 Pose 交给独立 evaluator 和 processor。

1. 主线程捕获输入，使用 register_tree 或 register_player 注册有序 Pose。
2. 每个 handle 每次 submit 前只 queue 一次。queue_update 是 sample/display/exact 全真的便捷入口。
3. submit 完成 Prepare，按 parent/child 单元调度 worker；少于16单元或有效worker只有1时同步执行。workers是选择的调度上限，实际执行线程与重叠使用下列累计指标判断。
4. complete / complete_batch 只等待：不写场景，不发布 method/audio，不发延迟动画信号。
5. publish(handle) 发布单项但不结束整帧；complete_and_publish / finish_all 等待并按依赖顺序发布、收尾。重复发布不重复事件。
6. unregister_animation（或兼容别名 unregister_tree）丢弃待发布输出并恢复旧 callback mode。注册不重启原 Tree/Player 播放状态。

Pose 输出查询等待它的已注册 producer，但不发布事件，也不强制精确重采样。它的 final/display 输出与 Skeleton3D 的 public base 输出是两种语义：发布阶段临时呈现 base→pose_updated、final→skeleton_updated/attachments/skin，最后恢复 public base。base/final 时间历史使用对齐的插值时刻。

pending 帧和发布调用在需要期间持有 processor 强引用；idle controller 不永久自持。注销与销毁不能硬取消正在读取的 worker，必须 join 后清理。RefCounted 在后台 finalizer 中销毁时只排主线程 scene-cleanup payload，SceneTree 帧收尾/显式 drain 再恢复场景所有权。确定性交回控制请在主线程显式注销/release，不依赖 GC 的具体时刻。

捕获通过 ObjectID 识别节点，不接管节点生命周期。回调可以删除/注销/重定向目标；发布在每个 callout 后校验对象身份、capture generation 和输入版本，失效输出会被拒绝或重新求值。资源/场景必要写前 hook 同步读者；外部调用仍应遵守主线程变更合同。

当前共享纯值求解路径覆盖受支持的 BoneRotationOffset、CopyTransform、TwoBoneIK 与零duration LookAt。脚本/自定义重写、时间型 LookAt、show_rest/有效deprecated global override、同骨同channel重复track，以及不支持的离散/脚本属性或嵌套动画组合会被拒绝捕获或同步fallback；这不是把未知行为静默忽略。不兼容 Skeleton observer 会禁止延迟时间插值并强制采样/展示，本身不等于整个native路径fallback。普通图、root motion、blend-shape、method/audio仍按既有Mixer语义执行相应阶段。以实际fallback reason/counters验证具体图是否走native，不把类型注册成功当作全部输入都受支持。

可变 Pose/Skeleton 不能属于两个独立注册。跨 Pose 的 copy/attachment 来源必须无环，且是同一注册内更早的 Pose 或显式 parent 单元。未表达顺序的平行 entry 不允许互读。

F01实际任务统计在每batch独占slot记录一次 `Thread::ID`、主线程标记和整个 `_execute` 的elapsed，join后只归并一次。`main_thread_tasks_total + worker_thread_tasks_total == tasks_total`；这里计真实进入回调的batch，含没有prepared entry可计算的batch，不是角色数。`task_threads_total`是该Processor生命周期内见过的唯一线程ID数，包含inline主线程；线程池可在不同提交中使用不同worker，所以累计唯一数可以超过某次requested workers，窗口差值为0也不表示该窗口没有worker执行。

每次Submit重置running/peak原子量，`task_concurrency_peak_last`在尚未join的新提交中暂为0，join后为该次 `_execute` 活动区段最大重叠数；空提交为0、inline为1。`task_concurrency_peak_total`保留历史最大值，`parallel_submissions_total`只在已join提交的peak>1时增加一次。这些累计值不受详细诊断开关影响，重复complete/publish及注销不重计或清零。每batch只增加常数原子操作和一次线程ID采集，A/B两侧开销相同；重叠是elapsed区段重叠，不是CPU核心利用率或硬件同时运行证明，`task_wall_us_total`仍是非嵌套wall work sum，不能与其内部Pose阶段重复相加。

## 精确采样、时间 LOD 与骨骼 LOD

queue_update_with_options(handle, delta, sample, display, exact, time, interval, force_full_bones=false) 的 delta 始终推进图；sample=false 只请求跳过骨骼采样。time 是调用者单调 presentation 秒数，interval 是采样间隔/展示延迟。两次有效兼容历史可在延迟时刻插值；缺历史、输入变更、不兼容 observer 或 interval=0 可强制采样。

exact=true 请求 sample/display 并丢弃本次展示延迟，和完整骨骼是两个独立维度。force_full_bones=true 临时使用完整拓扑，不改 configure_bone_lod / set_bone_lod 的配置级别，也不改 Game distance/time LOD。

configure_bone_lod 接收显式 bone_names 与 first_disabled_lod。数组等长、名称非空且唯一、首次禁用级别>=1；LOD0完整。缺名在捕获时失败，不模糊匹配、不启动加载模型猜组。空配置始终完整。祖先与 modifier/socket/copy/attachment 所需骨保留；compact 求值后用 inactive rest 展开到原始完整 Skin palette。Game 的 CZBoneLodProfile 是独立纯配置 Resource，不能把 body profile 套给不同骨架的 head/hair。

set_bone_lod_enabled 与 set_pose_simd_enabled 改全局设置前完成已有帧，同值快速返回。SIMD enabled 只表示请求，需 pose_simd_available 判断当前构建/平台内核是否可用；double 等不支持路径回退 scalar。两条路径保持 Godot Quaternion.slerp、rest 顺序和缺轨语义，不用 nlerp 代替。

F04历史问题（native-thread10）：当时仅在LOD0把force_full_bones归一化为false。LOD1及更高级别的false→true→false会使 `prepare_frame` 的changed和capture的rebuild成立；每次rebound清历史并增加layout_version，Bridge随后清空该Pose的binding.tracks，遍历整个animation_set重新reset/解析/finalize。F05的CZBoneLod描述缓存命中不会阻止这次轨道重新绑定，实际映射碰巧相同时也没有额外豁免；这不等于官方AnimationTree节点图必须重建。Game精确开火路径显式传true，正常展示路径传animationCollisionCorrection；Exact单独不会设置ForceFullBones。

本次源码使用一份完整order轨道绑定，LOD/ForceFull变化只重建active prefix视图及reference/mask，真正rest、拓扑和依赖变化仍使绑定失效。reference沿用官方Mixer的init_*：Skeleton骨骼使用Skeleton rest；仅没有Skeleton rest的Node3D轨道才可由RESET首键覆盖。回归夹具必须区分二者：Finger rest=4、RESET=12、Move=8、权重0.5时，官方与候选的完整骨骼结果应为6，低LOD骨local应为rest4；显式把两侧rest改为2并清理Mixer缓存后，保留该骨的结果应为5，低LOD仍为rest2。不能把RESET=4与默认rest0误算成完整结果6，也不能把inactive骨的local假定为上次完整姿态。

`native-stable-lod11-failed`保留124项中123项通过、该新增夹具两条期望错误的原始失败记录。修正非零rest夹具并增加官方对照/CAPTURE后，`native-stable-lod12`统一原生筛选124/124用例、307856/307856断言通过，见[测试日志](G:/ColonyZeroGoDot/TaskArtifacts/cz-modules-4.8/resume-20261105/native-stable-lod12/tests.stdout.log)。本次没有降低容差或改CPP实现；该结果覆盖对应Windows x64/float构建的正确性，不是LOD1性能收益证据。

旧native-thread10的[单次LOD1诊断](G:/ColonyZeroGoDot/TaskArtifacts/cz-modules-4.8/resume-20261105/thread10-lod1-probe01-summary.json)为60.004秒、NativeErrors=0、全帧mean116.74ms、Prepare41.20ms/completed frame、track_descriptor_builds增598266。它帮助定位反复绑定，不能与另一次完整骨骼probe直接构成配对收益，也不能替代修复后五组A/B。

`force_full_bones_requests_total`是合法true entry Queue的持久累计，包含碰撞请求，不是切换次数。`capture_rebounds_total`/`capture_input_refreshes_total`仍按当前work中Pose的lifetime值求和，成员未queue或注销时可能下降，不能直接当持久窗口计数；`captured_bones`是本帧重绑及普通输入刷新的捕获量。诊断时结合 `prepare_us_total`、`track_descriptor_builds` 和真实活动/完整骨数，不能仅因method_validation_scans为0排除模块轨道重建，因为官方Mixer track_map_version未必变化。

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

get_skeleton_buffer_statistics(reset=false) 默认只读已被 backend 消费的原子计数；reset=true 原子消费。RD 输出 apply_calls/bytes、bulk_apply_calls、per_bone_apply_calls、apply_elapsed_us、upload_passes、dirty_skeletons、upload_calls/bytes、skin_dispatches/vertices、reshape_dispatches。不支持统计的后端（包括当前 GLES 默认实现）返回空 Dictionary，不能将缺失指标伪造为0。比较真实上传时需另控制 frame_post_draw / queue sync；这仍不是 GPU completion 计时。

F12的 `CZ_ANIM_SKIN_UPLOAD_PER_BONE=1` 保留同一完整palette和同一次call_on_render_thread，在RD/GLES3内部逐骨重建Transform3D并调用官方skeleton_bone_set_transform；默认bulk路径则整段memcpy并标脏。两侧都由Callable保留COW payload，遵守RID resize/上传/free的队列顺序；Dummy接口接受相同参数但不提供真实GPU证据。该A/B隔离的是backend apply方式，不会恢复旧的逐骨RenderingServer公开命令，因此不能据此声称主线程命令数量减少。

RD的apply_calls每份非空palette计一次，apply_bytes两侧均按完整palette字节数；bulk_apply_calls计实际整段写，per_bone_apply_calls计实际逐骨setter调用，apply_elapsed_us计对应完整apply区段elapsed。这些属于渲染线程已执行的CPU区段，不是GPU完成时间；upload/dispatch继续按真实后续区段计数，不能把apply次数当作上传或dispatch次数。

F13仅处理Forward+中显式声明 `render_mode depth_prepass_no_material` 的surface，在DEPTH_NORMAL_ROUGHNESS及其VOXEL_GI预通道使用默认材质shader，以几何法线/默认粗糙度替代该材质预通道结果；颜色和阴影pass不受该开关影响。`CZ_ANIM_DISABLE_NO_MATERIAL_DEPTH_PREPASS=1`保留原材质预通道，开关在进程首次使用时固定、每render list入口读取，不在每surface查询环境变量。`no_material_depth_eligible_draw_calls_total`和`no_material_depth_simplified_draw_calls_total`在真实draw调用后记录，分别计符合声明的调用及实际采用简化路径的调用；每个非空合格render list只汇总一次原子计数，两侧相同开销。它们是全进程累计draw调用数，不是surface/实例/顶点数或GPU时间，正式收益仍需对应GPU pass配对测量。

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

前次交付的Windows native016统一筛选为104/104用例、106188/106188断言；double及Tracy各104/106188，CZ全关闭21/210，原声明LookAt missing_target warning保留。两项#if0旧内部谓词档案不计入运行数。上述数字对应native-frame-eventless-freeze-016.json，不覆盖本轮新增实现。Game/图形/平台和性能证据仍分别列范围，原生通过不等于完整计划验收。macOS ARM64/NEON尚未实机运行。

匹配45-callback SDK配套的Game015已执行1327项普通测试（1320通过、7个历史失败方法保留）及156项Godot逻辑测试。两条声明native ERROR使严格适配器仍返回false，原始结果保留；没有新增错误。真实CLR退出pin/join/drain有旧负对照1次和新正对照3次，不能据此宣称原稀发交错已精确复现或所有用户后台线程均受治理。

### 2026-11-05继续实施：正确性与50v50状态

最新 `native-stable-lod12` 为Windows x64/float、Mono、profiler=none构建，统一原生筛选124/124用例、307856/307856断言通过，保留声明的LookAt missing_target warning；日志见前节及同目录stderr/build.json。此前 `native-thread10` 为122/122用例、304003/304003断言通过；`native-shared-lod-thread09`另保留SoA组合和AoS/zero-off组合日志，两组各122用例通过。上述是对应构建/配置的正确性证据，不代表全部开关组合、渲染后端或平台都已验证。

`thread10-b02`使用冻结native-thread10与combat100-new-source07，world 50v50、AnimalCount=0、Seed=20260910、预热15秒、持续窗口60.08秒，Strict50v50Valid=true、NativeErrors=0、NativeWarnings=6；完整记录见[单次探针摘要](G:/ColonyZeroGoDot/TaskArtifacts/cz-modules-4.8/resume-20261105/thread10-b02-probe-summary.json)和[原始capture](G:/ColonyZeroGoDot/TaskArtifacts/cz-modules-4.8/resume-20261105/combat100-new-source07/.temp/combat-profile/thread10-b02/world-50v50-high-normal-off-1/capture.json)。该probe记录900个display frame，全帧mean为66.75ms、逻辑14.98 ticks/s、GameTimeToRealRatio约0.2497，TimingPassed/Target120Passed均false；Strict50v50Valid不表示达到了60逻辑ticks/s。这是一个有效工作量探针，不能称为性能通过、提升或相对基线的收益。

| 项目 | 当前证据与限制 | 本轮50v50性能结论 |
| --- | --- | --- |
| F01集中驱动/并行 | 原生回归；b02窗口main任务增390、worker任务增11700、parallel_submissions增900、历史peak=8，证明实际线程分配及区段重叠 | 仅一轮probe，未完成五组配对及C/O对照，不报告提升 |
| F02/F03 AoS/SoA与SIMD | 有原生标量/布局/内核正确性回归；b02为SoA、SIMD有效 | 缺独立五组A/B |
| F04骨骼LOD | Native12有prefix/父闭包/依赖/无效输入及ForceFull切换不重绑回归；旧Native10 LOD1单次诊断发现绑定热点，已据此修改源码，见前节 | 未通过性能验收，修复后仍需LOD1及切换的五组配对 |
| F05静态轨道与CZBoneLod共享 | 有共享、隔离、回收回归；b02共享开关OFF；SkeletonAnimationPose::Bone/reference_pose仍未共享 | 未测共享ON的五组100人容量/生成A/B，不称F05整体完成 |
| F06采样权重scratch | 有逐clip/独占/缺通道等正确性回归；b02复用OFF；只覆盖三组采样权重，不是所有事件/临时分配 | 未测独立五组A/B |
| F07首份原地blend | 有AoS/SoA、scalar/SIMD、边界数学回归；b02 OFF；不减少临时Pose数量 | 未测独立五组复制量/耗时A/B |
| F08零贡献/lazy部分 | 原生正确性回归；b02 ON，仍推进时钟、事件/root motion；weight=1快捷copy未实现 | 仅部分实现，缺独立五组A/B |
| F09按输出需求 | 既有P09/Siroino生成库及Rifle输入库静态清点没有BlendShape/Value/Bezier/Audio轨道，Method音频通知保留，见[静态调查](G:/ColonyZeroGoDot/TaskArtifacts/cz-modules-4.8/resume-20261105/f09-inventory/README.md)；未实现空跳过开关 | 该夹具没有候选工作量，未交付、未验收 |
| F12骨骼buffer | b02 Forward+ bulk模式完整palette读回与队列顺序444项校验通过，17/20骨、QueueSentinelConsumed=true；不是GPU完成/dispatch或主线程命令计数证明 | bulk单次正确性通过，per-bone及GPU后续探针不纳入本表；缺五组A/B |
| F13预通道材质退出 | b02 eligible/simplified draw各增102600，说明有真实目标调用；该probe未采GPU pass timing | 不以draw次数或全帧单次mean判定收益，缺GPU pass五组A/B |

本轮没有哪项性能feature仅凭这些正确性结果或单次probe完成验收。正式结论仍要求同场景A/A噪声、每项至少五组配对A/B、实际工作量/开关校验，以及相对C与O双基线的证据；主指标未可靠改善则不进入默认优化交付。当前默认ON的既有行为只表示保留原行为，不表示已通过新协议。

取数时，持续阶段累计量使用 `Phases[sustained].NativeAnimationStatisticsAtEnd - AtStart`，不要使用含启动/预热的最终AnimationStatistics总量。capacity是当前快照，不取累计差来冒充分配或峰值；F05/F06需比较同一生成/稳态检查点的有效容量，具体scope见下节。当前collector保存阶段起终与每秒native字典，但GetStatistics不隐式Complete，pending时容量键缺失，缺失不是0；生成阶段目前没有原生计数起终，SpawnMeasurements的时间/托管分配不能替代原生绑定或scratch分配证据。

### Pose fastpaths and controlled comparison

Pose fastpaths are enabled by default. `CZ_ANIM_DISABLE_POSE_FASTPATHS=1`, read once when a processor is constructed, selects the previous sampling/weight-copy/track-prefix flow for a controlled comparison. This internal diagnostic switch does not revert correctness fixes, change graph time, events, SIMD selection, LOD, or worker scheduling. It does not add a ClassDB or managed API.

For deterministic mixing, the sampler reads each instance's raw weights directly. Event-bearing/Player paths own a snapshot through event publication; the strict eventless Tree path described below borrows only during Evaluate. Non-deterministic mixing still uses owned normalized weights. A sampled Pose/instance initializes its scratch pose and channel weights only when the first nonzero effective channel successfully interpolates. If no channel writes, it leaves the blended pose unchanged. Scratch contents are private and unspecified for such a call. Track order, the original near-zero test, and ordered quaternion interpolation remain unchanged; an instance is never skipped solely because its scalar weight is zero.

Within module evaluation/publication, early track skipping retains enabled/cache/index checks and the original non-deterministic zero-total-weight behavior. It updates root-motion classification before skipping a track, then computes weights only for surviving tracks. The official synchronous and module-disabled flows retain their original order. Root-motion path changes synchronize pending work and invalidate binding/reference/history caches; changing the local-root-motion option also synchronizes pending work.

With `CZ_ANIM_PHASE_DIAGNOSTICS=1`, completed statistics additionally report actual animation instances, mixer track-count sums, raw weight elements (including Player first-key snapshots), normalized-copy elements, and sampler calls/zero-weight/no-written/empty-binding calls. These are counts, not memory-bus traffic or timing. They reset per Submit, include dirty publication re-evaluation, and are absent before completed publication or with diagnostics disabled. Existing phase timers include their diagnostic overhead; do not use an enabled diagnostic run as formal whole-frame acceptance evidence.

Only deterministic fastpaths can additionally certify an instance's exact +/-0 weights for the union of all registered Poses' actual bone blend indices while copying its owned snapshot or inspecting its current borrowed view. Non-finite instance weights, nonzero/non-finite masked elements, invalid indices or an unprepared mask use normal sampling; non-deterministic and disabled-fastpaths flows do not use this certificate. Masks are built during existing binding reconstruction and unioned only after a binding change, never by a steady-frame animation-library rescan. First-key and event-bearing instances remain owned; auxiliary/root-motion work and per-Pose evaluation remain intact.

Completed phase diagnostics additionally expose `sample_certified_zero_weight_calls` (a subset of existing zero-weight/no-written calls, retaining the old call/empty-binding counts) and `zero_weight_mask_rebuilds` (actual union reconstructions, normally zero on unchanged warmed frames). These counts are not a performance promise. The behavior regressions passed in the matching native016 unified build/run.

累计统计不受详细诊断开关影响：`task_wall_us_total` 在 join 后累加整批 `_execute`（含 inline）的非嵌套执行区段墙钟，属于 wall work 而非 CPU 时间；四个 Pose 阶段时间在 join/发布后按注册基准的累计差值计入 Processor，注销后仍保留，`active_bones_total`/`full_bones_total` 按真正进入 Evaluate 且需要 sample/display 的 Pose 计活动/完整骨数各一次，`sampled_bones_total`/`displayed_bones_total` 分列实际需要对应工作的活动骨数（均含 dirty 重算，均非上传量），`force_full_bones_requests_total` 仅计合法接受的 true entry Queue 请求且不包含 Exact，主线程 Prepare/Wait/Publish 及 completed frame 在整帧收尾时只累加一次（空 Submit 也完成一帧）。

### 静态轨道描述共享

`CZ_ANIM_SHARED_TRACK_DATA=1` 在进程内固定启用静态轨道描述共享，默认关闭、等待100人独立对照；F05的轨道部分共享三组有序轨道索引及 compact count/max blend index/valid，CZBoneLod描述共享范围见下节，SkeletonAnimationPose的Bone和reference_pose仍独立。每个 binding 仍完整验证和解析后才按完整内容 hash 加逐字段比较入池，允许不同 Animation 共享相同纯索引；Animation 引用和三组权重 scratch 仍各实例独立，修改共享描述前先复制。关闭时使用同一种描述而不入池。pool仅在主线程处理，在绑定清理/注销后回收仅pool持有的描述，模块退出清空。`shared_track_data_enabled` 为实际开关；`track_descriptor_live_count/live_bytes` 统计当前描述对象及三组 Track 数组实际 capacity（不含整进程RSS、pool/统计表、allocator元数据和独立scratch），`track_descriptor_builds` 计本地/COW描述实际构造，`track_descriptor_cache_hits/misses` 只计启用时的实际入池查找；构造和查找计数为进程累计。

这里的live_count/live_bytes是全进程当前唯一TrackData快照，包含不同Processor和仍由cache持有的描述，不是累计字节或仅本Processor注册实例。只有builds/hits/misses是进程累计。共享发生在完整本地解析后的finalize，cache hit不会省去此前的轨道遍历/解析，不能把命中次数称为避免绑定生成次数。正式比较应分别报告当前唯一容量与相同阶段的累计构造/查找差值。

### CZBoneLod只读描述共享

沿用进程固定的 `CZ_ANIM_SHARED_TRACK_DATA`，默认OFF；OFF和ON都使用同一个内部RefCounted描述类型，只有ON将完整描述入池。描述持有CZBoneLod原来的parents、source_rest、阈值/required、共同parent-first顺序、当前级别的连续前缀映射及展开数据。key包含完整parents/rest/阈值/required当前顺序和level/force_full_bones，hash命中后逐字段深比较；不按Skeleton实例版本猜测相等。build只在主线程执行，先在新描述上验证/生成，成功后替换引用，共享后的更新不改其它Pose；非法输入保留当前描述。仅level/force变化时复用当前描述的共同顺序和有效阈值，不重新拓扑排序。worker只读，不共享SkeletonAnimationPose::Bone、enabled、skin_scale、reference_pose、播放权重或历史姿态。

pool仅由主线程在实际build替换、既有绑定清理/注销点回收仅pool引用的描述，模块退出清空；描述析构不操作全局表，因此最后一个Pose引用可在原有允许的线程释放。没有raw live registry、通用对象池或稳定帧全库扫描。`get_layout_build_count()`继续表示该wrapper共同布局输入的成功变化次数（包括复用其它实例已生成描述），跨level/force不增加；`lod_layout_descriptor_builds`另外记录实际临时描述构造次数，包含最终因非法层级而丢弃的构造；`lod_layout_cache_hits/misses`只计启用共享时实际找到完整相同描述/成功构造并入池的次数，均为进程累计。

仅在主线程且没有pending/group/prepare/publish的GetStatistics完成点扫描稳定注册Pose，按descriptor identity去重输出 `registered_lod_layout_bytes/count`；同时输出 `lod_layout_pool_cached_bytes/entries` 和 `lod_layout_pool_only_bytes/entries`。bytes均含描述对象及其数组实际capacity，pool_cached与registered会重叠，不能相加；pool_only表示当前只由cache持有的额外描述容量，尚未到下一次清理点时也据实报告。注册范围不包括仍由其它未注册Pose持有的非缓存描述，均不含pool hash桶/引用数组、allocator元数据或整个进程RSS；采集本身有扫描和identity集合成本，工作尚未完成时这些键缺失。原 `pose_memory_bytes` 仍逐Pose累加引用到的描述容量，共享时会重复计算同一描述，不能用它声称F05实际内存下降；共享LOD主证据使用唯一注册容量，并分别列出cache容量。100人共享率、生成成本和性能收益尚未测量，默认OFF。

### 采样权重 scratch 复用

`CZ_ANIM_REUSE_SAMPLE_SCRATCH=1` 在进程内固定启用每 Pose 独占的三组采样通道权重复用，默认关闭；ON 时 binding reset 不分配权重，由主线程绑定阶段预分配当前 Pose 的 padded bones，worker 逐 clip 顺序复用，OFF 仍使用各 binding 自己的相同三数组。未显式传入 scratch 的内部直接调用安全回退到该 binding 自己的缓冲，不使用全局或 thread_local 缓冲；scratch 不被事件发布或上传引用，written/零权重/NaN/尾部初始化规则不变。`sample_scratch_reuse_enabled` 为实际开关，`sample_scratch_buffer_growths_total` 是这些三数组经 resize 发生实际 capacity 增长的进程累计次数（每组单独计，不等于全部原生分配或RSS）；`binding_sample_scratch_bytes`/`pose_sample_scratch_bytes` 只在主线程无 pending/group/prepare/publish 时于 GetStatistics 扫描稳定注册绑定和Pose统计实际 capacity，未完成时键缺失，采集本身有扫描成本，均不含allocator元数据；Pose scratch动态容量只计入pose_memory一次，binding权重容量另列。

F06当前容量以同一完成点的 `sample_scratch_bytes` 比较，它等于binding_sample_scratch_bytes与pose_sample_scratch_bytes之和；两者都是本Processor稳定注册实例的当前保留容量，不是全进程累计或瞬时峰值。不要再把pose_memory_bytes与pose scratch重复相加。buffer_growths_total涵盖进程内binding与Pose这些三组数组的扩容，包括内部安全回退，但不覆盖其它原生/托管分配或事件key查询scratch。worker区段、SampleBlend、Prepare和尾帧仍需分别检查，不用扩容次数推导FPS收益。

### 首份混合原地计算

`CZ_ANIM_IN_PLACE_FIRST_BLEND=1` 在进程内固定启用首份混合原地计算，默认关闭；每个 Pose 的 sampled 为独占且该 clip 消费后无其它消费者，首份实际执行的 blend 在 sampled 中按相同顺序计算 `rest + (sample - rest) * weight`，四元数沿用 `Animation::interpolate_via_rest(rest, sample, weight, rest)` 并使近零 lane 保留 rest，再 SWAP 给 blended，后续 clip 正常累加；沿用原采样器缺失通道初始化，不增加替代复制，若所有 clip 均未执行 blend 最后才复制 reference。关闭时保留最初的 blended/reference复制；legacy fastpaths=false 的全零和0*NaN仍完整执行原数学，不做weight=1直接copy。`in_place_first_blend_enabled` 为实际开关，`in_place_blends_total` 经 Entry 累计快照计实际首份原地blend次数（含dirty重算及legacy零权重运算），SWAP保持全部Pose复制计数之和，`pose_copy_bytes_total` 与SampleBlend为主要对照指标；临时Pose数量与容量没有减少，不宣称内存下降，事件和图时钟流程不变。

### 零贡献处理的独立对照

`CZ_ANIM_DISABLE_ZERO_POSE_SKIP=1` 进程内固定关闭现有零贡献跳过、采样懒初始化和零权重证书，实际键为 `zero_pose_skip_enabled=false`；默认ON保持已交付行为，不表示100人性能已通过。关闭时 sampler 使用原 `p_fastpaths=false` 初始化/混合数学（含全零及0*NaN），不构建/合并/扫描zero mask，同时保留独立的整段轨道证明，使borrowed weights、auxiliary track/instance skip等原bundle行为继续；图时钟、方法事件和Root Motion不随零骨骼贡献跳过，F07组合仍遵守相同blend数学。`sampler_calls_total`/`sampler_blends_total` 经每Pose非原子计数与Entry快照累计实际binding调用/实际blend，差值 `sampler_without_blend_calls_total` 包含证书、空绑定、无key、未写入等情况，不能全称为零权重；原诊断计数仍遵守原开关和每Submit范围。这里只是F08的零贡献/lazy部分，未实现单一权重1直接copy，也不宣称整个F08完成。

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

前次2026-11-05交接记录：当时专用crowd整帧54组有效，64/256所有模式三轮通过原5%门槛；4人分段LOD有两项超限（LOD1 r1=1.137456、LOD3 r1=1.053980）。当时详细CPU阶段仅完成11/45，后续和预注册同窗口4人18组取消，不能宣布完整性能验收。这些历史规模/门槛不替代本轮50v50五组配对及双基线协议。历史模型/插值/深度/空PhysicalBone数据按其原冻结版本保留。完整交接在Game/docs/WIP，不发布主Editor或改共享资产。
