# tool-exception-dump 技能评测用例集（RED-GREEN）

本文件包含14个RED-GREEN评测用例，用于验证`tool-exception-dump`技能的诊断正确性：Phase 0至Phase 5每阶段至少1个专项用例，另含2个端到端用例与1个真机日志用例；GREEN段的步骤级判读断言合计覆盖SKILL.md全部31个编号步骤（P0.1至P5.5，覆盖映射表见文末）。关键行文案一律抄录自本技能SKILL.md对应步骤的判读规则原文；fixture相关的实测口径（退出码、输出串、结果码）以oam-tools测试断言为据并逐处标注path:line，不虚构fixture中不存在的内容。

## 通用约定

- **RED**：无skill时AI的典型失败模式，即拿到同样输入后可能做出的错误动作、错误判读或含糊结论，须具体可辨。
- **GREEN**：有skill时AI按步骤P*.n推进的预期行为与判定输出，以"步骤P*.n：断言…"形式给出正/反判读断言；正断言规定必须发生的动作与判读，反断言规定必须避免的误判。
- **fixture根**：`oam-tools/test/st/msaicerr/res/ori_data/`（路径相对oam-tools仓库根）。用例输入只引用该目录下真实存在的文件与目录结构，或给出明确构造方法。
- **退出码语义**：退出码0或101-107为分析正常完成（101-107即分析结论码），1-11为工具状态码（SKILL.md P2.2）；结论码语义为101单算子运行错误、102内存分配错误、103硬件错误、104算子输入数据错误、105框架未执行memset清零、106算子args被踩、107atomic算子溢出（SKILL.md P3.5）。

---

## 用例1（P0）：ASCEND_OPP_PATH缺失拦截与环境恢复

**场景描述**：用户提供了收集目录要求分析AI Core Error，当前shell未source CANN（Compute Architecture for Neural Networks，异构计算架构）环境变量脚本，`ASCEND_OPP_PATH`为空。

**输入**：收集目录`oam-tools/test/st/msaicerr/res/ori_data/asys_output_20230713074104794/`（真实fixture，即随测试交付的真实故障数据集）；环境构造：新开shell未执行`source <CANN安装路径>/set_env.sh`。

**RED（无skill时的典型失败模式）**：AI不做任何环境检查，直接在任意目录执行`python3 msaicerr.py -p <收集目录> -out <结果目录>`，工具在解析参数前即报`Environment variable not set after the CANN software is installed.`并返回退出码2；AI不认识该文案，误判为CANN安装损坏，建议用户重装软件包，或换一个-p路径反复重试（每次仍退出码2），多轮无效后给出"环境异常，无法分析"的含糊结论。

**GREEN（有skill时的预期行为与判读）**：

- 步骤P0.1：断言先确认运行前提：本技能运行于故障发生环境本地、python不低于3.7.5、非异地环境且非Ascend RC形态，通过后才进入P0.2；环境变量缺失不构成本步终止条件，但也不得跳过本步确认直接执行分析命令。
- 步骤P0.2：断言先执行`echo "$ASCEND_OPP_PATH"`判空；输出为空时按判读规则预期工具行为：报`Environment variable not set after the CANN software is installed.`并返回退出码2（`oam-tools/src/msaicerr/msaicerr.py:290-293`；ST（系统测试）断言`test_environment_invalid`置空该变量后退出码为2，`oam-tools/test/st/msaicerr/testcase/test_msaicerr_st.py:153-159`）；失败分支动作为重新`source <CANN安装路径>/set_env.sh`后复检，复检通过方可继续；反断言：不得把退出码2报告为工具bug，也不得在未复检的情况下继续后续步骤。
- 步骤P0.3：断言环境恢复后先执行`python3 msaicerr.py -e -dev <device_id>`自检：正常输出5行`[INFO]`、末行为`The built-in sample operator runs successfully, The environment is normal.`且退出码0（ST断言`test_check_env_success`，`oam-tools/test/st/msaicerr/testcase/test_msaicerr_st.py:196-207`）；自检失败时退出码103并提示`See the detailed error logs above`，该失败构成103（硬件错误）方向的证据，环境恢复前不执行-p分析；反断言：不得跳过自检直接进入收集阶段。
- 步骤P0.6：断言执行预检：`pwd`不得位于-p目录或其子目录内、`touch debug_info.txt && rm -f debug_info.txt`验证当前目录与debug_info.txt可写、`mkdir -p <结果目录>`且结果目录不得为-p目录或其子目录；工具侧禁令文案为`Do not run msaicerr in the directory specified by -p or its subdirectory. Make sure -out specifies a different directory (including its subdirectory) from -p.`，违反时退出码2；当前目录或debug_info.txt不可写时报`The current directory or debug_info.txt is immutable, Please check.`退出码2；预检不过则更换执行目录或结果目录后重试，不得带病进入P1。
- 步骤P0.7：断言汇总Phase 0结论：`ls debug_info.txt`核验P0.3自检执行痕迹；诊断报告"环境与输入"段如实记录"ASCEND_OPP_PATH曾缺失、经source恢复"；环境自检通过、无不支持算子、位置约束满足、输入形态明确四项齐备才允许进入P1.1。

**涉及步骤号**：P0.1、P0.2、P0.3、P0.6、P0.7。

---

## 用例2（P0）：暂不支持算子拦截（MemSet）

**场景描述**：用户报障时给出了算子名MemSet，收集目录已就绪，须在进入分析前完成名单拦截。

**输入**：算子名构造为`MemSet`；收集目录沿用`oam-tools/test/st/msaicerr/res/ori_data/asys_output_20230713074104794/`（承载日志）。

**RED（无skill时的典型失败模式）**：AI不知道8个暂不支持分析的算子名单，照常进入收集与分析流程：执行-p长时间无有效产出，或凭日志中的aicore error字样给出"UB（Unified Buffer，统一缓冲区）越界、建议检查算子内存访问"的臆测根因；用户按建议排查数日无果，因为该类算子本就暂不支持分析，任何"根因结论"都无从验证。

**GREEN（有skill时的预期行为与判读）**：

- 步骤P0.4：断言清点输入四要素（故障日志、收集目录、Device ID、算子名），算子名MemSet与收集目录一并列入输入清单；缺项时按求助模板逐项询问（模板为含至少3个待补输入项的结构化列表），不得猜测输入继续。
- 步骤P0.5：断言用名单比对命令拦截：`echo "<算子名>" | grep -iE "MatmulAllReduce|AllGatherMatmul|MatmulReduceScatter|GroupedMatmulAllReduce|^MemSet$|NonMaxSuppressionBucketize"`，MemSet命中`^MemSet$`；命中即停止分析并如实告知用户该算子暂不支持；完整8项清单为MatmulAllReduce类算子、MatmulAllReduceAddRmsNorm、MatmulAllReduceInplaceAddRmsNorm、AllGatherMatmul、MatmulReduceScatter、GroupedMatmulAllReduce、MemSet、NonMaxSuppressionBucketize（`oam-tools/docs/zh/msaicerr/msaicerr_functions_and_restrictions.md:8-16`）；反断言：其余7项任一命中（如GroupedMatmulAllReduce、NonMaxSuppressionBucketize）同样拦截，不得强行继续分析（NEVER清单第5条）。

**涉及步骤号**：P0.4、P0.5。

---

## 用例3（P1）：收集目录三要素缺dump的降级

**场景描述**：已有收集目录但缺dump数据，须在进入分析前完成三要素校验、按分支处理输入形态并打降级标记。

**输入**：`oam-tools/test/st/msaicerr/res/ori_data/asys_output_20230713074104794/`，真实结构：`dfx/graph/370_0/`（含`ge_proto_00000000_Build.txt`等）、`dfx/ops/0/`（含`te_assign_d623a1e1b515a45cdc8c9658e58e2860034dbfbd9ab35f92e1415a0fda9d35c1_1.json`、`te_gatherv2_657cb48fa1743a43209d7bc779fe8c294760a5b09b3079a3323fdf18376fc408_1.json`、`cube_random_buff.h`、`vector_random_buff.h`，无.o文件）、`dfx/log/host/cann/debug/plog/`（多个plog）与`dfx/log/host/cann/run/plog/plog-370_20230713074109952.log`；目录内无`dfx/data-dump`。

**RED（无skill时的典型失败模式）**：AI不校验三要素直接执行-p，工具提取不到有效信息返回退出码8，AI不认识该退出码，反复重试并把8当工具故障上报；或把`dfx/graph`下的ge_proto文件当作dump数据；或见无data-dump目录即宣布"无法分析"终止，不给任何降级路径与替代方案。

**GREEN（有skill时的预期行为与判读）**：

- 步骤P1.1：断言输入形态判定：`ls -d <输入路径>/dfx`命中即收集目录形态，走分支A转P1.3；同一步骤须覆盖另两个分支的判定：输入为`*.tar.gz`走分支B转P1.2，只有报错现象、无目录无压缩包走分支C转P1.4；路径不存在或形态不明时按求助模板向用户确认，不得猜测路径继续。
- 步骤P1.2：断言（分支B变体，构造：把该fixture打包为tar.gz）必须手动执行`mkdir -p <解压目录> && tar -xzf <压缩包路径> -C <解压目录>`，且`ls <解压目录>`恰好列出一个asys_output_*子目录才以该子目录为收集目录转P1.3；多于一个子目录时工具侧get_select_dir会抛ValueError（`oam-tools/src/msaicerr/msaicerr.py:84-89`），须向用户确认哪个是有效收集目录，不得自行挑选。
- 步骤P1.3：断言按msaicerr查找链校验输入：`ls <收集目录>/dfx/data-dump/`失败（目录不存在）、`ls <收集目录>/dfx/ops/`非空、`ls <收集目录>/dfx/log/host/cann/`非空、`find <收集目录> -name "ge_proto_*_Build.txt"`命中GE图；缺dump时先在plog中检索IDEDD组件日志：`grep -nE "IDEDD.*dump (exception|host kernel|kernel json|kernel) to file" <plog文件或目录>`，并从`file: <路径>`字段提取实际落盘路径；命中后先核验该路径并按工具口径补采，不得直接询问用户；本fixture无dump落盘记录，转P1.5打降级标记后继续；反断言：plog存在时不得误判"缺plog"而停止分析（缺plog才停止并要求重新收集）；plog中已有落盘路径时不得未检索就询问用户；GE图缺失不阻断主分析但须降级标注。
- 步骤P1.4：断言（分支C变体）无任何收集时先执行`asys collect --task_dir=<编译缓存目录> --tar="True" --output=<输出前缀>`准确归集既有产物，随后转P1.3按msaicerr查找链校验plog、dump、算子编译信息与GE图；采集后缺dump时，先在plog的IDEDD日志中检索`dump ... to file, file: <路径>`并提取实际落盘路径，命中后按工具口径修正task_dir或环境变量后重跑asys collect，不直接询问用户；无记录或路径不存在时再询问用户遗漏文件位置；缺算子编译信息、GE图或其他文件时同样先从日志推导，再询问用户；仍不足时询问用户是否执行业务复跑，用户确认后要求其提供完整前台复跑命令（可前台执行、可等待退出，不接受后台任务）并执行`asys launch --task="<用户提供的完整前台复跑命令>" --tar="True" --output=<输出前缀>`；一键分析执行`asys analyze -r=aicore_error`（自动收集后内部直接调msaicerr -p）。执行asys collect/analyze前必须核对6个环境变量（ASCEND_PROCESS_LOG_PATH、NPU_COLLECT_PATH、DUMP_GRAPH_PATH、ASCEND_WORK_PATH、ASCEND_CACHE_PATH、ASCEND_CUSTOM_OPP_PATH）与业务运行时一致，不一致先警示用户并要求确认，不得静默继续；为保证解析数据准确性，复现前建议先清理日志；反断言：用户已有足够信息时不得直接要求复跑，采到目录后不得跳过msaicerr输入校验，plog已有dump落盘路径时不得未检索就询问用户，业务复跑必须先经用户确认并索取完整前台复跑命令。
- 反断言（工具边界）：.o/.json/.cce的查找、匹配与收集只通过asys工具和msaicerr工具完成；AI不得自行find/grep系统目录、编译缓存或OPP安装目录，只能在工具产出目录dfx/ops或collection/compile内做三要素核验。
- 步骤P1.5：断言降级标记：缺dump标记"缺dump"，P4改走构造数据路径且不得基于无dump证据下104结论；`ls <收集目录>/dfx/ops/*.o <收集目录>/dfx/ops/*.json 2>/dev/null`中.o无命中（该fixture只有.json无.o）时加打"定位降级"标记，报错行定位置信度降低；SK（SuperKernel，多算子融合编译形态）场景（`grep -rln "Begin to dump callback exception" <收集目录>`命中）只生成host.o，同样定位降级；工具侧收集失败或日志无法提取AI Core Error信息时写出空结论报告并返回退出码8（`oam-tools/src/msaicerr/ms_interface/aicore_error_parser.py:1819-1824`），见到退出码8即按"输入信息不可用"处理（该fixture在ST中-p的断言结果即为8，仅mock反编译与dump结果，日志提取路径未被mock，`oam-tools/test/st/msaicerr/testcase/test_msaicerr_st.py:102-114`）；反断言：见到退出码8不得盲目重复执行-p，须回P1.3复核三要素与日志时间窗。

**涉及步骤号**：P1.1、P1.2、P1.3、P1.4、P1.5。

---

## 用例4（P2）：L1场景判定与报错行定位

**场景描述**：L1场景（plog含dev_func与tiling_data）下的场景判定、-p执行与info.txt报错行判读。

**输入**：`oam-tools/test/st/msaicerr/res/ori_data/collect/l1/`（仅`collection/plog/plog-17890_202506017074109552.log`）；该plog真实含L1证据行`[AIC_INFO] dev_func:GatherV3_7869a97190b9b4d296d9414a005b954b_high_performance`、tiling证据行`[AIC_INFO] tiling_data:0x5a 0x28 0x00 0x00 ...`与报错行`there is an exception of aivec error, core id is 10, error code = 0, dump info: pc start: 0x12c100014fd0, current: 0x12c10001580c, ...`。

**RED（无skill时的典型失败模式）**：AI把dev_func行当普通INFO忽略，不做场景判定；以为收集目录必有exception dump可解析，四处找dump文件；把报错行中的`error code = 0`直接解读为"未发生硬件错误"，草草结束分析。

**GREEN（有skill时的预期行为与判读）**：

- 步骤P2.1：断言三条grep判定：`grep -rn "\[AIC_INFO\] dev_func:" <收集目录>`命中即L1（该fixture实测命中）、`grep -rn "fftsplus task execute failed" <收集目录>`无命中、`grep -rn "Begin to dump callback exception" <收集目录>`无命中，即L1且非ffts、非SK；判定只依赖日志文案命中与否；记录L1影响：无有效dump时P4转构造数据路径；实测口径印证：该fixture在ST中parse_level=1、结果码0（`oam-tools/test/st/msaicerr/testcase/test_aicore_error_parse_st.py:199-234`参数化含`collect/l1/`）。
- 步骤P2.2：断言在msaicerr.py所在目录执行`python3 msaicerr.py -p <收集目录> -out <结果目录> -dev <device_id>`；成功判据为终端打印`Analysis info is saved in <info.txt路径>`，结果目录形态为`<out>/info_{执行时间戳}/aicerror_0_{错误时间戳}/info.txt`；退出码0或101-107为正常完成，1-11为工具状态码；退出码8回P1.3复核三要素与日志时间窗，退出码2回P0.6检查路径约束，不得盲目重复执行-p。
- 步骤P2.3：断言读info.txt第③段Operator Error Line Number：Original Info给出`start pc`、`current pc`、`Error occurred most likely at line: <汇编偏移>`，偏移为十六进制无0x前缀、是估算值而非源码行号；L1场景偏移行之后跟随`<反汇编文件名>:<err_pc>`、cce行号、TBE源码`文件:行号`三级信息（L1才有后两级）；出现Corrected Info块时优先按其定位（修正PC由AI Core错误寄存器回填，比原始PC更可信）；反断言：不得把汇编偏移直接当TBE源码行号报告给用户。

**涉及步骤号**：P2.1、P2.2、P2.3。

---

## 用例5（P2）：ffts场景dump失败与102判读

**场景描述**：ffts场景且dump拷贝失败（D2H failed），须正确分类dump失败、关联结论码102并联动P4路径决策。

**输入**：`oam-tools/test/st/msaicerr/res/ori_data/collect/ffts/`（`collection/plog/plog-test.log`与`collection/plog/plog-1592007_20240912164003456.log`）；plog-test.log真实含`fftsplus task execute failed, device_id=0, stream_id=42, report_stream_id=42, task_id=1, flip_num=0, fault kernel_name=FlashAttentionScore_5881aeec01e51adb01fb1db8be1c04f0_10000000000022420943_mix_aic, ...`与`[Dump][Exception] D2H failed`。

**RED（无skill时的典型失败模式）**：AI把`D2H failed`当host与device间通信故障，建议查链路、网卡或驱动；不知道dump失败与结论码102的对应关系，也不知道P4因此没有dump输入可用；对ffts场景没有"警惕D2H failed"的意识，仍尝试对data-dump目录执行-d解析。

**GREEN（有skill时的预期行为与判读）**：

- 步骤P2.1：断言`grep -rn "fftsplus task execute failed" <收集目录>`命中即ffts场景标志成立（该fixture实测命中）；三个标志（L1、ffts、SK）相互独立、可同时为真，判定只依赖日志文案命中与否。
- 步骤P2.6：断言dump失败分类与多错误判读：`grep -rn "D2H failed" <收集目录>`命中（该fixture实测命中）即dump失败、结论码102（内存分配错误方向），P4无dump输入可用、改走路径B；info.txt第⑤段出现`Failed to get dump data of error op!`或`Dump exception.*failed`、`the address maybe invalid`任一特征命中同样判dump失败；多错误场景工具按日志时间只解析第一个AI Core Error，修复首个错误后仍有报错须重新收集走一轮P1至P5；反断言：不得把102当作工具自身bug报告，也不得在dump失败后仍对data-dump目录执行-d解析；实测口径印证：该fixture在ST中parse结果102、ffts_flag=True（`oam-tools/test/st/msaicerr/testcase/test_aicore_error_parse_st.py:202-207`，参数化注释"日志中含有D2H failed标识dump数据失败"），且collect校验实测不通过（`oam-tools/test/st/msaicerr/testcase/test_collection_st.py:73`）。
- 步骤P4.1：断言从msaicerr -p生成的info.txt第⑥段读取自动复现结果，不重新执行单算子复现；dump失败场景下自动复现结果按第⑥段三态文案判读，缺dump只影响输入来源，不得据此停止结果判读；反断言：不得在msaicerr已生成结果时默认重跑单算子。

**涉及步骤号**：P2.1、P2.6、P4.1。

---

## 用例6（P2）：符号化缺失的llvm-symbolizer与objdump回退链

**场景描述**：自动符号化失败（llvm-symbolizer未安装或.o无调试信息），须走手动回退链定位报错行。

**输入**：构造：基于`oam-tools/test/st/msaicerr/res/ori_data/asys_output_20240912164014958/`的收集目录执行-p后，假设info.txt第③段Corrected Info出现`Unable to calculate the corrected line number, check the logs for more details.`；环境构造：`which llvm-symbolizer`无命中。回退链第4步的_loc.json结构参照真实fixture`oam-tools/test/st/msaicerr/res/ori_data/complie_path/kernel_meta/te_transdata_a603142389973648_c50cf465894adb0d_1_loc.json`（内为cce_full_path与cce_line2loc数组，数组元素含cce_line与loc字段）。

**RED（无skill时的典型失败模式）**：AI遇到`??`或行号缺失提示即报告"无法定位报错行"结束；或建议`apt install llvm`安装不相干的工具包；不知道CANN工具链目录自带llvm-symbolizer；完全不知道cce-objdump五步定位链，最终交付一份无定位结论的报告。

**GREEN（有skill时的预期行为与判读）**：

- 步骤P2.3：断言行号缺失提示`Unable to calculate the corrected line number, check the logs for more details.`出现即判符号化失败，转P2.4手动回退；无Corrected Info块时以Original Info估算偏移转P2.5反汇编链；反断言：不得停留在"待确认"而不给回退动作。
- 步骤P2.4：断言手动回退顺序：先`which llvm-symbolizer`，未命中则到CANN工具链目录查找（如`/usr/local/Ascend/latest/<arch>-linux/ccec_compiler/bin/`）；找到后执行`llvm-symbolizer -obj=<kernel的.o文件> 0x<修正后偏移>`（偏移取info.txt第③段Corrected Info，.o路径取第①段kernel file字段）；正常输出两行，即函数名与`源文件:行:列`；输出`??`或`??:0:0`即该.o无调试信息、符号化失败；未安装态工具文案为`llvm-symbolizer is not installed.`；反断言：手动回退仍输出`??`时不得编造行号，转P2.5。
- 步骤P2.5：断言五步链执行：`cce-objdump -d --line-numbers <kernel.o> > <kernel.o>.txt`反汇编（llvm-objdump为备选）、`grep -n "<err_pc>:" <kernel.o>.txt`（err_pc为第③段偏移，十六进制无0x前缀）、从报错行向上找最近`cce:<行号>`、`grep -n "\"cce_line\": <cce行号>" <kernel_name>_loc.json`映射TBE源码`文件:行号`（_loc.json结构见上述真实fixture）；断链点（.o与.json都不存在、无cce行号、_loc.json缺失、L0到偏移为止）均为正常降级，保留已有级结果；`cce-objdump`与`llvm-objdump`均不可用（`No cce-objdump or llvm-objdump found.`）时按工具建议把cce-objdump与.o复制到其他机器执行`cce-objdump -d <kernel_name>.o > <kernel_name>.o.txt`；反汇编文本报错行含PIPE_ALL时给出set_flag/wait_flag配对检查提示（检查算子代码中set_flag与wait_flag是否配对使用）；反断言：L0场景只给汇编偏移属正常，围绕偏移与10条指令上下文判读，不得硬造cce行号。

**涉及步骤号**：P2.3、P2.4、P2.5。

---

## 用例7（P3）：David芯片错误位判读（逗号十进制错误码）

**场景描述**：David芯片（ASCEND_950）错误码为逗号分隔的十进制位号，须按David字典解码且判型方向不得写反。

**输入**：构造：plog报错行`error code = 65, 361`，dump info后随`sc error info:`、`su error info:`、`l1 error info:`寄存器键；解码依据`oam-tools/src/msaicerr/ms_interface/constant.py:378-1123`（David字典共187条，实测可查：65为MTE_NDDMA_REG_BUF_ECC、361为VEC_ERR_DIRTY_ECC_MBERR_T0）。

**RED（无skill时的典型失败模式）**：AI把`65, 361`当成一个整数或误当十六进制串，按Stars的176位字典做二进制展开，套算出完全错误的错误位名与根因方向；或判型方向写反（把0x形态判David、逗号十进制判Stars），整份报告的芯片结论与错误位结论全部错误。

**GREEN（有skill时的预期行为与判读）**：

- 步骤P3.1：断言两法判型：方法一按错误码形态，以0x开头的十六进制指向Stars（ASCEND_910B），逗号分隔的十进制位号指向David（ASCEND_950），方向不得写反（`oam-tools/src/msaicerr/ms_interface/aic_error_info.py:25-40`）；`65, 361`判David；裸`0`两芯片同形，只能落到Stars路径走trap_or_timeout兜底；方法二按dump info寄存器键：含`sc error info:`、`su error info:`、`l1 error info:`指向David，含`ifu error info:`、`ccu error info:`、`biu error info:`指向Stars；两法冲突以方法二为准；两法都判不出时按Stars路径兜底处理并注明判定置信度降低。
- 步骤P3.2：断言David解码：错误码即绝对位号列表，直接查David字典，65解码为MTE_NDDMA_REG_BUF_ECC、361解码为VEC_ERR_DIRTY_ECC_MBERR_T0；位号按模块分段：CUBE从0、MTE从64、L1从128、SC从192、SU从256、VEC从320起；位号查不到时输出`unknown error bit <位号>`，如实记录不臆断含义；无位置位时输出兜底文案`trap_or_timeout, timeout or trap error`，这不是位0，不能直接读成"确认超时"；反断言：不得把位号65按Stars十六进制展开解码，不得在无位置位时编造错误位名。
- 步骤P3.3：断言David不走六模块子域分析，模块信息直接读错误名前缀（CUBE_、MTE_、L1_、SC_、SU_、VEC_）：MTE_NDDMA_REG_BUF_ECC前缀即MTE模块；反断言：不得对David报文提取`vec error info:`等子寄存器串做Stars式子域解读。

**涉及步骤号**：P3.1、P3.2、P3.3。

---

## 用例8（P3）：Stars错误位解码与0x0三元组特殊场景

**场景描述**：Stars芯片（ASCEND_910B）十六进制错误码的位展开解码、六模块子域分析与错误码0x0的v300三元组特殊场景。

**输入**：`oam-tools/test/st/msaicerr/res/ori_data/collect_milan/`（`collection/plog/plog-588579_20250402071926352.log`、`collection/AddCustom_ab1b6750d7f510985325b603cb06dc8b.json`、`collection/DirtyCustom_ab1b6750d7f510985325b603cb06dc8b.json`）；plog真实含报错行`there is an aivec error exception, core id is 16, error code = 0x200000000, dump info: pc start: 0x12c0c002e000, current: 0x12c0c002e5e8, ...`（后随vec、mte、ifu、ccu等寄存器串）、六模块子域串行`error info: 0x51000070f3, mte error info: 0xb90000006a, ifu error info: 0x29883d2080000, ccu error info: ...`与故障算子行`fault kernel_name=AddCustom_ab1b6750d7f510985325b603cb06dc8b_0`。

**RED（无skill时的典型失败模式）**：AI不做二进制位展开，把`0x200000000`笼统解读为"错误码值很大、问题严重"；对错误码0x0的报错直接判"无异常"结束；不知道extend info三元组可重建错误码；六模块子寄存器串整行忽略，丢失出错地址信息。

**GREEN（有skill时的预期行为与判读）**：

- 步骤P3.1：断言判型：方法一`0x200000000`以0x开头指向Stars（ASCEND_910B）；方法二dump info含`ifu error info:`、`ccu error info:`、`biu error info:`键同样指向Stars，两法一致。
- 步骤P3.2：断言Stars解码：把十六进制错误码按二进制展开为置位位号（`0x200000000`即2^33，置位位号为33），查Stars字典（176位，`oam-tools/src/msaicerr/ms_interface/constant.py:155-369`）：33为`mte_gdma_write_overflow`（MTE写片上buffer越界），根因方向为越界写；实测口径印证：该fixture在ST中parse结果0（兜底）、parse_level=0即L0（`oam-tools/test/st/msaicerr/testcase/test_aicore_error_parse_st.py:208`）。
- 步骤P3.3：断言六模块子域分析：对Stars命中的模块提取`<单元键>=(\S+)`子寄存器串解析错误类型与出错地址（该plog的vec、mte、ifu、ccu error info串为真实提取对象）；MTE按错误位动态选子字典（SOC、FMC、FMD、UNZIP、AIPP）；approximate地址是补零近似值仅供参考；VEC的repeats字段表示错误重复次数；子寄存器串取不到时输出`No <单元>_ERR_INFO found`属正常（plog可能只记录部分单元），靠主字典释义继续。
- 步骤P3.4：断言特殊场景判读：场景5错误码`0x0`不等于无异常，从extend info三元组重建错误码（判读样例取自真实no_cce fixture的`The extend info: errcode:(0, 0, 0) errorStr: timeout or trap error.`行，`oam-tools/test/st/msaicerr/res/ori_data/no_cce/asys_aicerror/asys_output_20250508095626087/dfx/log/host/cann/debug/plog/plog-155320_20250508095618528.log`），重建不出按trap_or_timeout兜底；场景1 atomic溢出要求错误码恰`0x800000`加`dha status 1`命中加memset检查缺失（AtomicLaunchKernelWithFlag未找到）三证据并存，结论按105优先；场景2 args前后改写即第④段args before与after不一致，指向106；场景3 outstanding双PC在主正则无命中时降级AICORE_ERR_OCCUR_OST，需确认使用与出错任务对应的PC；场景4并发isconcurrentexe去重后任务数超过2时放弃自动判定需人工分析，并发时atomic结论可能误报；反断言：本用例错误码0x200000000不等于0x800000，不得触发atomic溢出链。
- 步骤P3.5：断言核对报告Basic information段的`rts_block_dim`与`driver_aicore_num`：两者均有效且rts_block_dim大于driver_aicore_num的2倍时，结论为环境AI Core数量不满足算子并发需求；对照结论码语义表（101-107）后再进P4；反断言：0兜底不代表故障不存在（该fixture实测结果码0，须逐节核对六节报告证据，不得宣布"无问题"）。

**涉及步骤号**：P3.1、P3.2、P3.3、P3.4、P3.5。

---

## 用例9（P4）：读取自动复现结果与复现成功判读

**场景描述**：收集目录含真实exception dump，msaicerr已具备自动复现输入；本用例读取并判读其自动结果与输入有效性结论。

**输入**：`oam-tools/test/st/msaicerr/res/ori_data/asys_output_20240912164014958/`，真实结构：`dfx/data-dump/0/exception_info.42.1.1726159207469285`（exception dump文件）、`dfx/log/host/cann/debug/plog/plog-1592007_20240912164003456.log`（真实含`fault kernel_name=FlashAttentionScore_5881aeec01e51adb01fb1db8be1c04f0_10000000000022420943_mix_aic`与`[aicore exception]`字样）、`dfx/log/host/cann/run/plog/plog-1592007_20240912163952762.log`、`dfx/log/host/cann/run/device-0/device-1592007_20240912164001269.log`；无`dfx/ops`。该目录在ST的collect校验中实测通过（`oam-tools/test/st/msaicerr/testcase/test_collection_st.py:70`标注"验证正常流程"）。

**RED（无skill时的典型失败模式）**：AI拿到dump不会用`-d`：对.bin产出缺`-dtype`报错后放弃；把.npy文件直接喂给`-d`被拒后判定工具损坏；复现判读时把复现日志里任何aicore error字样都当复现成功，不知道kernel名共现要求与RetCode.FAILED的语义反转，把其他算子的错误算到目标算子头上。

**GREEN（有skill时的预期行为与判读）**：

- 步骤P4.1：断言读取msaicerr自动复现结果：`grep -n "Execution Result of the Single-Operator Test Case" <info.txt路径>`定位第⑥段，再检索三种结果文案；本fixture收集目录含exception_info文件说明工具具备路径A输入，但仍以info.txt自动结果为准，不重新执行dump解析或单算子复现；反断言：不得在自动结果存在时另行执行-d或重跑单算子。
- 步骤P4.1补充：从info.txt第⑤段检索`Input data invalid`与`Input data maybe invalid`。NaN/Inf为强证据，0.9倍值域为弱证据，须结合业务值域确认；两提示只支撑104，不得据此臆断101或103；均无命中时dump校验链判有效，不得臆造104。`-d`是独立解析入口，仅当用户要求单独解析dump或msaicerr结果缺失时使用，本用例不执行。
- 步骤P4.2：断言三态与越界判读：第⑥段命中`The AI Core error was reproduced...`即复现成功（RetCode.FAILED）；`...was executed, but the AI Core error was not reproduced.`即未复现（RetCode.SUCCESS，不代表算子无问题）；`...was not executed...`即未运行（RetCode.NOT_RUN）。复现成功的底层判据是同一日志文件内错误串与目标kernel名共现，但该判定已由msaicerr完成，主流程读取结果文案而非重扫日志；未复现走升级路径；未运行计入101方向。越界判读以magic_ret与tail遍sync_ret为主；`Access Memory without OverBoundary`无当前代码产出方，运行时侧输出仅作佐证；反断言：不得仅凭日志含aicore error关键字就认定复现成功。

**涉及步骤号**：P4.1、P4.2。

---

## 用例10（P4）：自动结果NOT_RUN时的手动复现回退

**场景描述**：L1场景无dump数据，msaicerr自动单算子结果为NOT_RUN或结果缺失，需评估是否执行手动复现回退。

**输入**：`oam-tools/test/st/msaicerr/res/ori_data/collect/l1/`（仅`collection/plog/plog-17890_202506017074109552.log`，无dump目录、无编译产物）；该plog真实含tiling证据行`[AIC_INFO] tiling_data:0x5a 0x28 0x00 0x00 ...`。

**RED（无skill时的典型失败模式）**：AI无dump即宣布"无法复现"结束诊断；不知道tiling可从plog重建，构造数据时tiling传空导致kernel行为失真；用全零数据填充输入，不知道设备内存与UB初值不为0、工具会用脏UB预写暴露依赖初值的隐性缺陷，复现结果不可信。

**GREEN（有skill时的预期行为与判读）**：

- 步骤P4.1：断言先读取info.txt第⑥段；自动结果为NOT_RUN或缺失时才进入回退评估，不得默认重新执行单算子复现；反断言：不得因无dump而直接重跑复现。
- 步骤P4.3：断言回退前置条件：自动结果缺失、NOT_RUN或用户明确要求补充验证；回退前向用户说明触发原因并确认。若执行手动复现，`ASCEND_WORK_PATH`作为第一优先级环境变量指向独立运行目录，stdout/stderr用tee留存，命令退出后立即快照`${ASCEND_WORK_PATH}/log`；重试使用新时间戳目录。自动结果存在时以未重编译的自动流程结果优先，手动结果仅补证并在报告中单独记录；反断言：用户未确认或自动结果已充分时不得执行手动复现。

**涉及步骤号**：P4.1、P4.3。

---

## 用例11（P5）：四证据链汇聚与104优先级判定

**场景描述**：dump校验证据链命中输入数据无效，四链汇聚后按优先级链输出104，复现未成功不推翻结论。

**输入**：构造：在用例9路径A的基础上，dump解析输出日志含`input[0] NaN/INF. Input data invalid. Please check!`（构造方法：按P4.3判读规则与`oam-tools/src/msaicerr/ms_interface/dump_data_parser.py:184-218`的提示行格式构造该行）；三变体执行结果为未复现（RetCode.SUCCESS）。

**RED（无skill时的典型失败模式）**：AI的结论随证据漂移：先据NaN/Inf说算子实现bug（101），被追问又说硬件异常（103）；因复现未成功而推翻dump证据，改口"无法定位"；不按任何优先级规则，自创"综合错误码110"之类的结论；报告没有证据链与置信度，用户无法复核。

**GREEN（有skill时的预期行为与判读）**：

- 步骤P5.1：断言四链汇聚：反编译→101（出错在算子源码哪一行）、寄存器解码→103（硬件报了哪一类异常，辅以golden op（标杆算子）环境自检）、dump校验→104（输入数据本身是否已脏）、单算子复现→105-107（脱离模型能否稳定复现、args有没有被踩）；本场景dump校验链填入NaN/Inf命中事实并支撑104，复现链填入"未复现"事实；某条链数据缺失时明确写"本链失效（原因）"，不允许留空或臆造；反断言：dump数据无效只支撑104证据链，不得据此臆断101、103（NEVER清单第3条）。
- 步骤P5.2：断言优先级链判定：优先级链105→107→104→106→102→101→103→0，先命中先返回（`oam-tools/src/msaicerr/ms_interface/aicore_error_parser.py:1963-1980`）；本场景无框架memset缺失（105未命中）、无atomic溢出（107未命中），命中104；复现未复现不推翻104（链上104先于101，未复现须走P5.4升级而非改写结论）；四个判读细节：107在返回码层面实际不可达（atomic_add_err求值前提含atomic_clean_check为假，会先命中105，两条件互斥）、NOT_RUN同样命中101、flag_check与AI Core数量不足与current_pc为0x0没有专属返回码只进结论文案、结论文案链与返回码链排序不同（人工读文案、脚本读返回码时预期到差异）。
- 步骤P5.3：断言修复建议映射：按四元组（触发条件、根因方向、修复建议、验证方法）查映射表输出；结论码104触发输入数据非法方向，建议检查上游数据生成与预处理链路；结论码触发与错误位触发同时存在时以结论码行为主、错误位行为根因细化；映射表未覆盖且无法按已覆盖根因方向（越界读写、UB越界绕回、原子溢出、输入数据非法、地址未对齐、ECC硬件类）就近归类时，只给排查方向不给确定性根因，并注明依据不足。
- 步骤P5.4：断言升级路径：复现未复现时输出四条路径：用msSanitizer工具再次检查算子、开启op_debug_config=oom内存错误检测后复跑检查、SK场景关闭算子融合后重跑让故障以单算子形式复现（四链全部生效）、怀疑框架问题时联系技术支持上报；反断言：复现失败场景只给排查方向，不给确定性根因（未复现什么都没证明）。
- 步骤P5.5：断言报告输出并落盘：六段齐备（环境与输入、场景与定位、错误类型、复现结果、证据链汇聚判定、修复建议与升级路径）；每个诊断结论包含事实、对比（判据出处）、判定、置信度四要素并附反证；置信度口径：高为结论码由判定字段直接触发且至少两条证据链相互印证，中为仅一条链支撑或存在可解释反证，低为0兜底或证据链大面积缺失只输出排查方向；本场景104由dump校验链单链直接触发、复现链未复现构成可解释反证，置信度应为中并在反证段如实呈现；报告必须保存为`.md`文件，回复用户时必须告知该文件的绝对路径。

**涉及步骤号**：P5.1、P5.2、P5.3、P5.4、P5.5。

---

## 用例12（端到端）：asys_output_20240912164014958全流程（含真实exception dump）

**场景描述**：端到端全流程：含真实exception dump的L0故障，从P0.1环境检查推进到P5.5六段式报告。

**输入**：`oam-tools/test/st/msaicerr/res/ori_data/asys_output_20240912164014958/`（完整真实结构见用例9；补充证据行，均出自`dfx/log/host/cann/debug/plog/plog-1592007_20240912164003456.log`）：报错行`The error from device(chipId:0, dieId:0), serial number is 87, there is an fftsplus aivector error exception, core id is 0, error code = 0, dump info: pc start: 0x12c042d73754, current: 0x12c042d75b18, vec error info: 0x99000000a2, mte error info: 0x5003000031, ifu error info: 0x200000007ffc0, ccu error info: 0x280d00000084, ...`、三元组行`The extend info: errcode:(0, 0, 0) errorStr: timeout or trap error.`、tiling源二证据行`[AIC_INFO] args(0 to 20) after execute:0xe7ffc0009000, ...`。

**RED（无skill时的典型失败模式）**：AI综合跑偏：cat日志找ERROR行后把`error code = 0`解读为"无错误"；见到`fftsplus aivector error exception`字样误判为ffts场景；忽略`dfx/data-dump`目录不解析dump；最后凭经验给出"疑似硬件故障，建议换卡重试"的臆测结论，全程无步骤、无证据链。

**GREEN（有skill时的预期行为与判读）**（分阶段断言，判读细则同前述专项用例，此处列端到端顺序与关键判读）：

- Phase 0：步骤P0.1确认本地分析与python版本；步骤P0.2source后`echo "$ASCEND_OPP_PATH"`非空并进入msaicerr.py所在目录；步骤P0.3执行`python3 msaicerr.py -e -dev 0`自检通过（末行`The built-in sample operator runs successfully, The environment is normal.`）；步骤P0.4清点输入（收集目录即本fixture、Device ID默认0、算子名未知留待工具提取）；步骤P0.5无已知算子名，名单比对不命中；步骤P0.6预检执行位置与可写性；步骤P0.7`ls debug_info.txt`核验痕迹并汇总入口。
- Phase 1：步骤P1.1`ls -d <输入路径>/dfx`命中走分支A；步骤P1.3三要素校验：`dfx/data-dump/0/`非空（exception dump在）、`dfx/ops`缺失（无.o/.json，定位降级）、`dfx/log/host/cann/`非空；步骤P1.5打"定位降级"标记后继续（plog在，不触发停止条件）。
- Phase 2：步骤P2.1三条grep：`[AIC_INFO] dev_func:`无命中判L0、`fftsplus task execute failed`无命中、`Begin to dump callback exception`无命中；反断言：不得因plog存在`fftsplus aivector error exception`字样而误判ffts场景（场景判定只认`fftsplus task execute failed`文案命中与否）。步骤P2.2执行-p并按`Analysis info is saved in <info.txt路径>`定位info.txt。步骤P2.3判读第③段：L0场景报错行定位到汇编偏移为止（三级信息L0只有第一级），`Error occurred most likely at line: <汇编偏移>`且偏移是估算值。步骤P2.4与步骤P2.5因无.o（fixture无dfx/ops）符号化与反汇编链断链，属正常降级：保留偏移级结果，按P2.4失败分支提示补齐dfx/ops编译产物后可重试，不得伪造cce行号。步骤P2.6三条dump失败grep（`Dump exception.*failed`、`D2H failed`、`the address maybe invalid`）均无命中，dump有效，直接转P3.1。
- Phase 3：步骤P3.1判型：错误码裸`0`两芯片同形只能落Stars走trap_or_timeout兜底，方法二dump info含`ifu error info:`、`ccu error info:`、`biu error info:`键指向Stars，两法一致。步骤P3.2无位置位，输出兜底文案`trap_or_timeout, timeout or trap error`，不能直接读成"确认超时"。步骤P3.3提取vec、mte、ifu、ccu子寄存器串做子域解读（真实串如`vec error info: 0x99000000a2`、`mte error info: 0x5003000031`）。步骤P3.4场景5：三元组行`The extend info: errcode:(0, 0, 0) errorStr: timeout or trap error.`重建失败，按无位置位兜底处理并注明置信度低。步骤P3.5核对`rts_block_dim`与`driver_aicore_num`并对照结论码语义表。
- Phase 4：步骤P4.1读取msaicerr自动复现结果与第⑤段输入有效性结论；不重复执行-d或单算子复现。步骤P4.2按第⑥段三态文案判读，复现成功底层判据是错误串与目标kernel名共现（本fixture目标kernel为`FlashAttentionScore_5881aeec01e51adb01fb1db8be1c04f0_10000000000022420943_mix_aic`），但主流程读取工具结果而非重扫日志。若自动结果缺失或NOT_RUN，步骤P4.3先向用户确认是否手动回退；自动结果充分时不执行回退。
- Phase 5：步骤P5.1四链逐链填写（反编译链因无.o失效须如实写"本链失效（原因）"）。步骤P5.2按优先级链105→107→104→106→102→101→103→0取最终结论码。步骤P5.3按结论码与错误位查映射表输出修复建议与验证方法。步骤P5.4仅在复现未复现或证据不足时输出四条升级路径。步骤P5.5输出六段式报告并保存为Markdown文件，每个结论附事实、对比、判定、置信度与反证，降级与证据不足如实标注，回复用户时告知报告绝对路径。

**涉及步骤号**：P0.1、P0.2、P0.3、P0.4、P0.5、P0.6、P0.7、P1.1、P1.3、P1.5、P2.1、P2.2、P2.3、P2.4、P2.5、P2.6、P3.1、P3.2、P3.3、P3.4、P3.5、P4.1、P4.2、P4.3、P5.1、P5.2、P5.3、P5.4、P5.5（共29步；P1.2与P1.4由用例3覆盖）。

---

## 用例13（端到端）：no_cce无cce文件降级全流程

**场景描述**：端到端降级路径：L1场景、无dump目录、ops有json无.o（无cce文件），定位链全程降级但分析不中断，最终输出如实标注链失效的报告。

**输入**：`oam-tools/test/st/msaicerr/res/ori_data/no_cce/asys_aicerror/asys_output_20250508095626087/`，真实结构：`dfx/ops/GatherV3_9e31943a1a48bf81ddff1fc6379e0be3_high_performance.json`与`dfx/ops/vendor_config/`（无.o文件）、`dfx/log/host/cann/debug/plog/plog-155320_20250508095618528.log`、`dfx/log/host/cann/run/plog/plog-155320_20250508095608125.log`（真实含L1证据行`[AIC_INFO] dev_func:GatherV3_9e31943a1a48bf81ddff1fc6379e0be3_high_performance`）、`dfx/log/device/dev-os-0/application/run/`下device-app-100809等设备侧应用日志目录；无`dfx/data-dump`。debug plog真实含报错行`there is an aivec error exception, core id is 0, error code = 0, dump info: pc start: 0x12c100014f8c, current: 0x12c1000157d0, vec error info: 0x51000070f7, mte error info: 0x206000046, ...`、三元组行`The extend info: errcode:(0, 0, 0) errorStr: timeout or trap error.`与故障算子行`fault kernel_name=GatherV3_9e31943a1a48bf81ddff1fc6379e0be3_high_performance_10330`。

**RED（无skill时的典型失败模式）**：AI见`dfx/ops`有json即认为可以反汇编定位，对info.txt中cce file字段为空无法解释；忽略run/plog里的dev_func行漏判L1；把"无dump目录"与"dump失败"混为一谈，误关联102结论；分析中断后给用户"全部重新收集"的粗放建议，不区分哪些要素缺失、哪些仍可用。

**GREEN（有skill时的预期行为与判读）**（分阶段断言）：

- Phase 0：步骤P0.1、P0.2、P0.3、P0.4、P0.5、P0.6、P0.7同用例12口径执行（环境确认、source与变量校验、golden op自检、输入清点、无已知算子名不命中名单、预检、汇总入口）。
- Phase 1：步骤P1.1`ls -d <输入路径>/dfx`命中走分支A；步骤P1.3三要素校验：`dfx/data-dump`不存在判缺dump、`dfx/ops`非空但只有json无.o、`dfx/log/host/cann/`非空；步骤P1.5打"缺dump"与"定位降级"双标记，继续P2（plog在，不停止）。
- Phase 2：步骤P2.1`grep -rn "\[AIC_INFO\] dev_func:" <收集目录>`命中（真实证据行在run/plog）判L1。步骤P2.2执行-p，成功判据为终端打印`Analysis info is saved in <info.txt路径>`（ST在mock反编译与dump结果后实测该输出并读取info.txt，`oam-tools/test/st/msaicerr/testcase/test_msaicerr_st.py:267-292`）。步骤P2.3判读第③段：L1期望三级定位信息，但本场景info.txt第①段cce file字段值为空（ST断言原文`'cce file          : \n'`，`oam-tools/test/st/msaicerr/testcase/test_msaicerr_st.py:291-292`），判读为定位降级并如实呈现；反断言：不得伪造cce行号或TBE源码行号。步骤P2.4.o不存在，失败分支为回P1检查编译产物收集（dfx/ops），补齐后重试本步。步骤P2.5反汇编链断链（.o不存在属断链点），保留已有级结果，向用户说明补齐.o后可重试。步骤P2.6三条dump失败grep无命中：本场景是"缺dump数据"（收集侧无data-dump目录，P1.5已标记）而非"dump失败"，不得误关联102。
- Phase 3：步骤P3.1错误码裸`0`落Stars走兜底，方法二dump info含`ifu error info:`等Stars键，两法一致。步骤P3.2兜底文案`trap_or_timeout, timeout or trap error`，不读成"确认超时"。步骤P3.3提取vec、mte、ifu子寄存器串做Stars子域解读（真实串`vec error info: 0x51000070f7`、`mte error info: 0x206000046`）。步骤P3.4场景5三元组（真实证据行`The extend info: errcode:(0, 0, 0) errorStr: timeout or trap error.`）重建失败按兜底处理并注明置信度低。
- Phase 4：步骤P4.1读取msaicerr自动复现结果；无dump只影响工具内部single_op输入，不触发主流程重跑。步骤P4.2按三态判读，目标kernel名为`GatherV3_9e31943a1a48bf81ddff1fc6379e0be3_high_performance_10330`。步骤P4.3仅在结果缺失或NOT_RUN且用户确认后手动回退。
- Phase 5：步骤P5.1反编译链失效如实写"本链失效（无.o编译产物）"，dump校验链失效如实写"本链失效（无dump）"；链大面积缺失时结论置信度降级为低，只输出排查方向。步骤P5.2仍按优先级链105→107→104→106→102→101→103→0取结论码，全部判据未命中返回0时逐节核对六节报告证据，0不代表故障不存在。步骤P5.4证据不足时输出四条升级路径。步骤P5.5输出六段式报告并保存为Markdown文件，降级与证据不足如实标注，反证段不得省略，回复用户时告知报告绝对路径。

**涉及步骤号**：P0.1、P0.2、P0.3、P0.4、P0.5、P0.6、P0.7、P1.1、P1.3、P1.5、P2.1、P2.2、P2.3、P2.4、P2.5、P2.6、P3.1、P3.2、P3.3、P3.4、P4.1、P4.2、P4.3、P5.1、P5.2、P5.4、P5.5。

---

## 用例14（真机日志）：Ascend910B4 CUBE参数非法错误的符号化链判读

**场景描述**：真机Ascend910B4环境故意触发的AI Core Error（样例kernel `_Z12error_kernelPDhS_S_` 以CUBE非法输出地址经asc_mmad指令报错），日志为runtime侧完整符号化链输出：PrintCoreInfo寄存器打印、host kernel落盘、Error register information、Fix PC与Error PC information、Error symbol、llvm-symbolizer源码行、classification与Group[0]汇总、rtStreamSynchronize返回码。

**输入**：内嵌真机日志逐字块（仅将用户主目录泛化为`~/`，符号/地址/行号/十六进制一律逐字保真；环境事实：SoC=Ascend910B4、CANN=/usr/local/Ascend/cann-9.3.0、driver 26.1.1、aarch64）。素材来源：Ascend910B4真机执行runtime仓4_adump_error_locate故障定位样例的完整日志（用户提供，2026-09-22）。

```text
[ERROR] RUNTIME(4028848,aicore_error_locate):2026-09-22-14:54:51.620.440 [stars_engine.cc:1624]4028877 ProcLogicCqReport:Task run failed, device_id=0, stream_id=47, task_id=0, sqe_type=0(ffts), errType=0x1(task exception), sqSwStatus=0
[ERROR] RUNTIME(4028848,aicore_error_locate):2026-09-22-14:54:51.635.189 [device_error_core_proc.cc:1138]4028877 AddExceptionRegInfo:add error register: core_id=24, stream_id=47, task_id=0
[ERROR] RUNTIME(4028848,aicore_error_locate):2026-09-22-14:54:51.635.208 [device_error_core_proc.cc:1227]4028877 PrintCoreInfo:An error occurs on the device(chipId:0, dieId:0), the serial number is 40, the error is aicore error, core id is 24, error code = 0, dump info: pc start: 0x124000000000, current: 0x124000000500, vec error info: 0, mte error info: 0xfb9bf383af, ifu error info: 0x69cf2dcae66c0, ccu error info: 0xe29d17e679cef0e, cube error info: 0x100003b, biu error info: 0, aic error mask: 0x6500020bd00028c, para base: 0x12c100000000, aic cond: 0. The extend info: errcode:(0, 0x4000000000000000, 0) errorStr: The CUBE instruction parameter is invalid. fixp_error0 info: 0xbf383af, fixp_error1 info: 0xfb, fsmId:0, tslot:0, thread:0, ctxid:0, blk:0, sublk:0, subErrType:4. For details, see the troubleshooting document on the Ascend official website. Search for the keyword "AI Core Error".
[ERROR] RUNTIME(4028848,aicore_error_locate):2026-09-22-14:54:51.635.291 [davinci_kernel_task_v100.cc:579]4028877 SetStarsResultForDavinciTask:AI Core kernel task execution failed, retCode=0x26.
[ERROR] RUNTIME(4028848,aicore_error_locate):2026-09-22-14:54:51.635.316 [davinci_kernel_task.cc:1030]4028877 PreCheckTaskErr:An error occurred in the kernel task, retCode=0x26, [aicore exception].
[ERROR] RUNTIME(4028848,aicore_error_locate):2026-09-22-14:54:51.635.395 [davinci_kernel_task.cc:839]4028877 GetArgsInfo:[AIC_INFO] args(0 to 2) after execute:0x12c0c0015000, 0x12c0c0016000, 0x12c0c0017000.
[ERROR] RUNTIME(4028848,aicore_error_locate):2026-09-22-14:54:51.635.405 [davinci_kernel_task.cc:847]4028877 GetArgsInfo:tilingKey = 0, print 1 Times totalLen=(3*8), argsSize=24, schemMode=0, blockDim=1, gridDim3=[0,0,0], blockDim3=[0,0,0]
[ERROR] RUNTIME(4028848,aicore_error_locate):2026-09-22-14:54:51.635.508 [davinci_kernel_task.cc:972]4028877 PrintErrorInfoForDavinciTask:[DFX_INFO]AI Core kernel execution failed, device_id=0, stream_id=47, report_stream_id=47, task_id=0, flip_num=0, fault kernel_name=_Z12error_kernelPDhS_S_, fault kernel info ext=none, program id=0, hash=13023285847820503829.
[INFO] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.635.561 [task_fail_callback_data_manager.cc:37] 4028877 Notify: notify [AdumpException] task start, notify task_id=0, stream_id=47, retcode:507015.
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.635.716 [exception_dumper.cpp:191][tid:4028877] [Dump][Exception] Begin to dump exception. dumpScene=aic_err_brief_dump, deviceId=0, streamId=47, taskId=0, exceptionType=2(aicore), kernelName=_Z12error_kernelPDhS_S_.
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.635.909 [kernel_info_collector.cpp:296][tid:4028877] [Dump][Exception] dump host kernel to file, file: ~/runtime/example/5_performance/adump/4_adump_error_locate/extra-info/data-dump/0/_Z12error_kernelPDhS_S__host.o
[INFO] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.635.983 [kernel_symbol_locator.cpp:109][tid:4028877] Parse kernel symbols success. parsedSymbolCount=6, normalizedSymbolCount=6, hasSymbolRange=1, minSymbolOffset=0x0, maxSymbolEnd=0x504, symbolTotal=803, accepted=6, nonFunc=797, invalidSection=0, invalidName=0.
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.636.317 [kernel_symbol_locator.cpp:688][tid:4028877] [Dump][Exception] Error register information. coreId=24, coreType=0, AIC_ERR_0=0x0 AIC_ERR_1=0x0 AIC_ERR_2=0x0 AIC_ERR_3=0x40000000 AIC_ERR_4=0x0 AIC_ERR_5=0x0 BIU_ERR_0=0x0 BIU_ERR_1=0x0 CCU_ERR_0=0x679cef0e CCU_ERR_1=0xe29d17e CUBE_ERR_0=0x100003b CUBE_ERR_1=0x0
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.636.323 [kernel_symbol_locator.cpp:688][tid:4028877] [Dump][Exception] Error register information. coreId=24, coreType=0, IFU_ERR_0=0xdcae66c0 IFU_ERR_1=0x69cf2 MTE_ERR_0=0x9bf383af MTE_ERR_1=0xfb VEC_ERR_0=0x0 VEC_ERR_1=0x0 FIXP_ERR_0=0xbf383af FIXP_ERR_1=0xfb AIC_COND_0=0x0 AIC_COND_1=0x0
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.636.331 [kernel_pc_fixer.cpp:102][tid:4028877] [Dump][Exception] Fix PC with module=CUBE, originalPC=0x124000000500, fixedPC=0x1240000004ec.
[INFO] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.636.337 [kernel_symbol_locator.cpp:709][tid:4028877] Correct startPC by kernel address. coreId=24, coreType=0, originalStartPC=0x124000000000, fixedStartPC=0x124000000000.
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.636.342 [kernel_symbol_locator.cpp:725][tid:4028877] [Dump][Exception] Error PC information. coreId=24, coreType=0, originalStartPC=0x124000000000, fixedStartPC=0x124000000000, originalCurrentPC=0x124000000500, fixedCurrentPC=0x1240000004ec, fixedPCOffset=0x4ec.
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.636.347 [kernel_symbol_locator.cpp:744][tid:4028877] [Dump][Exception] Error symbol information. coreId=24, coreType=0, symbol=_Z8asc_mmadPU3AS5fPU3AS3DhPU3AS4Dhttthbbb.cube+0xd4.
[INFO] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.636.506 [lib_path.cpp:67][tid:4028877] Get self library path: /usr/local/Ascend/cann-9.3.0/aarch64-linux/lib64/libascend_dump.so
[INFO] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.636.532 [kernel_source_symbolizer.cpp:186][tid:4028877] Locate llvm-symbolizer from CANN install path: /usr/local/Ascend/cann-9.3.0/aarch64-linux/bin/llvm-symbolizer
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.653.404 [kernel_source_symbolizer.cpp:674][tid:4028877] [Dump][Exception][Symbolize] llvm-symbolizer raw output for offset 0x4ec (total 94 bytes):
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.653.415 [kernel_source_symbolizer.cpp:676][tid:4028877] /usr/local/Ascend/cann-9.3.0/asc/impl/c_api/memory_base_impl/cube_compute_intf_impl.h:161:9
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.653.434 [kernel_symbol_locator.cpp:845][tid:4028877] [Dump][Exception][Symbolize] classification summary. cores=1, groups=1.
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.653.451 [kernel_symbol_locator.cpp:520][tid:4028877] [Dump][Exception][Symbolize] Group[0] summary (cores=1):
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.653.457 [kernel_symbol_locator.cpp:522][tid:4028877] Group[0] oFile=~/runtime/example/5_performance/adump/4_adump_error_locate/extra-info/data-dump/0/_Z12error_kernelPDhS_S__host.o fixedPCOffset=0x4ec symbol=_Z8asc_mmadPU3AS5fPU3AS3DhPU3AS4Dhttthbbb.cube+0xd4
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.653.466 [kernel_symbol_locator.cpp:522][tid:4028877] Group[0] outerSrc=/usr/local/Ascend/cann-9.3.0/asc/impl/c_api/memory_base_impl/cube_compute_intf_impl.h:161:9 innerSrc=/usr/local/Ascend/cann-9.3.0/asc/impl/c_api/memory_base_impl/cube_compute_intf_impl.h:161:9
[ERROR] IDEDD(4028848,aicore_error_locate):2026-09-22-14:54:51.653.471 [kernel_symbol_locator.cpp:522][tid:4028877] Group[0] cores=[{id=24,type=0}]
[ERROR] RUNTIME(4028848,aicore_error_locate):2026-09-22-14:54:51.653.742 [api_c_stream.cc:168]4028848 rtStreamSynchronize:ErrCode=507015, desc=[aicore exception], InnerCode=0x7150026
```

**RED（无skill时的典型失败模式）**：AI把PrintCoreInfo行中的`error code = 0`直接当"无错误"，忽略同行紧随的extend info三元组`errcode:(0, 0x4000000000000000, 0)`与`errorStr: The CUBE instruction parameter is invalid.`，草草判"设备未报错"结束；看到`Group[0] outerSrc`与`innerSrc`落在CANN安装目录的头文件`/usr/local/Ascend/cann-9.3.0/asc/impl/c_api/memory_base_impl/cube_compute_intf_impl.h:161:9`而非用户算子源文件，误判"定位失败/定位到无关行"，建议重新收集或宣布符号化结果无意义；把`Fix PC with module=CUBE, originalPC=0x124000000500, fixedPC=0x1240000004ec`的PC差值误读为"PC被篡改/二次异常"，围绕差值臆测内存踩踏；不知道`dump host kernel to file`行`_Z12error_kernelPDhS_S__host.o`落盘意味着收集目录已具备kernel二进制产物与dump侧素材（复现输入已在手），仍要求用户重新收集。

**GREEN（有skill时的预期行为与判读）**：

- 步骤P2.1：断言三组场景grep全不命中判L0：`[AIC_INFO] dev_func:`、`fftsplus task execute failed`、`Begin to dump callback exception`在日志与收集目录均无命中（ProcLogicCqReport行的`sqe_type=0(ffts)`是SQE类型字段而非ffts场景标志，场景判定只认`fftsplus task execute failed`文案命中与否）；判L0后报错行定位到汇编偏移/修正PC为止属正常，不因无cce行号判定位失败。
- 步骤P2.3：断言从`Error PC information. coreId=24, coreType=0, originalStartPC=0x124000000000, fixedStartPC=0x124000000000, originalCurrentPC=0x124000000500, fixedCurrentPC=0x1240000004ec, fixedPCOffset=0x4ec.`行读出修正后偏移fixedPCOffset=0x4ec：该行属PC修正引擎的ADUMP_FIXED_PC正则族日志（正则定义oam-tools/src/msaicerr/ms_interface/constant.py:1213-1218，判读规则见references/error-line-location.md「PC修正两引擎」），直接采用其中已修正好的fixedCurrentPC/fixedPCOffset并按core_id=24匹配本核记录；修正PC由AI Core错误寄存器回填，比原始PC更可信，originalPC=0x124000000500到fixedPC=0x1240000004ec的差值是CUBE模块PC修正的正常回填而非异常。
- 步骤P2.4：断言symbolizer定位判读：`Locate llvm-symbolizer from CANN install path: /usr/local/Ascend/cann-9.3.0/aarch64-linux/bin/llvm-symbolizer`表明llvm-symbolizer从CANN安装路径命中（`which llvm-symbolizer`未命中时到CANN工具链目录查找，确切目录以现场CANN安装为准）；对host.o偏移0x4ec的raw output给出`/usr/local/Ascend/cann-9.3.0/asc/impl/c_api/memory_base_impl/cube_compute_intf_impl.h:161:9`，属"函数名与`源文件:行:列`"的正常输出形态，未出现`??`即符号化成功。
- 步骤P2.5：断言源码落点判读：`Group[0] outerSrc`与`innerSrc`同为`/usr/local/Ascend/cann-9.3.0/asc/impl/c_api/memory_base_impl/cube_compute_intf_impl.h:161:9`，结合错误符号`symbol=_Z8asc_mmadPU3AS5fPU3AS3DhPU3AS4Dhttthbbb.cube+0xd4`（asc_mmad内联展开实现的.cube段+0xd4）判读为：真实错误落点在CANN内联头文件（asc_mmad的内联实现处），outerSrc=innerSrc同落点是内联展开语义下的正常结果，不是定位失败也不是无关行；反断言：不得因落点在CANN头文件而非用户算子源文件否定定位结果或要求重新收集。
- 步骤P3.1：断言芯片形态判定：PrintCoreInfo行`error code = 0`为裸0，两芯片同形，只能落到Stars（ASCEND_910B）路径走兜底（真机SoC=Ascend910B4，与Stars判定一致）；方法二dump info含`ifu error info:`、`ccu error info:`、`biu error info:`键同样指向Stars，两法一致。
- 步骤P3.3：断言六模块子域分析：从`Error register information`行与PrintCoreInfo行提取CUBE子域串（`CUBE_ERR_0=0x100003b`、`cube error info: 0x100003b`，CUBE_ERR_INFO取自`cube error info:`），结合`errorStr: The CUBE instruction parameter is invalid.`判CUBE模块错误类型为CUBE指令参数非法类；反断言：不得把AIC_ERR_3=0x40000000等其他模块寄存器值当主错误源（errorStr与CUBE寄存器已共同指向CUBE）。
- 步骤P3.4：断言场景5三元组重读：错误码`0x0`不等于无异常，对`The extend info: errcode:(0, 0x4000000000000000, 0) errorStr: The CUBE instruction parameter is invalid.`行执行三元组重读，结合errorStr与CUBE寄存器证据落CUBE指令参数非法方向；重建不出时按trap_or_timeout兜底并注明置信度低；反断言：不得因`error code = 0`判"无错误"终止分析，也不得把兜底文案直接读成"确认超时"。
- 步骤P4.1补充：断言复现config素材判读（仅供手动回退参考）：`dump host kernel to file`行`~/runtime/example/5_performance/adump/4_adump_error_locate/extra-info/data-dump/0/_Z12error_kernelPDhS_S__host.o`落盘说明收集目录已具备kernel二进制产物；`fault kernel_name=_Z12error_kernelPDhS_S_`、`args(0 to 2) after execute:0x12c0c0015000, 0x12c0c0016000, 0x12c0c0017000`、`tilingKey = 0`、`blockDim=1`与`hash=13023285847820503829`共同构成16键复现config的素材；反断言：不得在素材已齐备且自动结果存在的情况下要求用户重新收集或重跑复现。

**涉及步骤号**：P2.1、P2.3、P2.4、P2.5、P3.1、P3.3、P3.4、P4.1。

---

## 步骤覆盖映射表（31步自查）

| 步骤 | 提供正/反断言的用例 | 步骤 | 提供正/反断言的用例 |
| --- | --- | --- | --- |
| P0.1 | 用例1、12、13 | P2.1 | 用例4、5、12、13、14 |
| P0.2 | 用例1、12、13 | P2.2 | 用例4、12、13 |
| P0.3 | 用例1、12、13 | P2.3 | 用例4、6、12、13、14 |
| P0.4 | 用例2、12、13 | P2.4 | 用例6、12、13、14 |
| P0.5 | 用例2、12、13 | P2.5 | 用例6、12、13、14 |
| P0.6 | 用例1、12、13 | P2.6 | 用例5、12、13 |
| P0.7 | 用例1、12、13 | P3.1 | 用例7、8、12、13、14 |
| P1.1 | 用例3、12、13 | P3.2 | 用例7、8、12、13 |
| P1.2 | 用例3 | P3.3 | 用例7、8、12、13、14 |
| P1.3 | 用例3、12、13 | P3.4 | 用例8、12、13、14 |
| P1.4 | 用例3 | P3.5 | 用例8、12 |
| P1.5 | 用例3、12、13 | P4.1 | 用例5、9、10、12、13 |
| P4.2 | 用例5、9、12、13 | P4.3 | 用例10、12、13 |
| P5.1 | 用例11、12、13 | P5.2 | 用例11、12、13 |
| P5.2 | 用例11、12、13 | P5.3 | 用例11、12 |
| P5.4 | 用例11、12、13 | P5.5 | 用例11、12、13 |

## 评测评分标准

| 检查维度 | 权重 | 说明 |
| --- | --- | --- |
| 判读规则遵循 | 高 | GREEN断言的关键行文案、退出码、结论码与SKILL.md对应步骤判读规则一致 |
| fixture事实准确 | 高 | 用例输入引用的路径、目录结构、日志证据行真实存在，构造项明确标注构造方法 |
| 降级与失效如实标注 | 高 | 链失效、定位降级、缺dump等场景如实标注，不臆造定位或复现结果 |
| NEVER违反 | 高 | 任何用例的GREEN预期行为不得违反SKILL.md NEVER清单14条 |
| 反断言有效性 | 中 | 每个专项用例至少含1条反断言（明确必须避免的误判） |
| 报告契约完整 | 中 | 端到用例的预期输出覆盖六段式报告全部段落，并断言报告落盘为Markdown文件且告知绝对路径 |
