# MySQL9.0无备份环境下磁盘级灾难恢复逻辑研究

## 摘要

针对MySQL 9.0数据库在无逻辑备份、无物理备份或备份不可用条件下的数据抢救问题，本文研究一种不修改MySQL内核、仅依赖磁盘残存物理文件、InnoDB表空间及日志信息的磁盘级灾难恢复逻辑。首先分析MySQL 5.6/5.7传统FRM元数据路径与MySQL 8.0及以后SDI元数据路径的差异，进而构建融合文件系统残留发现、InnoDB物理页识别、B+Tree记录解析、Undo/Redo证据审计、BLOB链重组和字段语义解码的分层恢复流程。针对压缩页、JSON、加密页、ext4与XFS等复杂场景，本文采用证据分级和严格ground-truth比较策略，规定低置信度结果只能作为取证证据，禁止生成CSV或SQL。实验设计区分单元测试、合成数据、文件级验证与真实灾难实验，避免将候选页数量或内部分类结果误认为真实恢复率。研究表明，磁盘残留仍可为部分数据抢救提供依据，但物理扇区完全损毁、关键元数据缺失或日志链断裂时无法保证恢复；该工具只能作为应急抢救手段，不能替代备份、binlog和容灾体系。

**关键词：** MySQL 9.0；磁盘级恢复；InnoDB；SDI；FRM；B+Tree；Undo/Redo；文件系统取证

## 1 引言

### 1.1 研究背景

数据库系统发生误删除、误格式化、表空间损坏、文件系统异常或主机存储故障后，传统恢复通常依赖物理备份、逻辑备份、binlog、远程副本或容灾节点。当上述材料不存在、不可读取或恢复链条不完整时，仍可能有部分数据以物理残留形式存在于表空间、共享表空间、日志文件、文件系统空闲区域和已解除链接的文件中。此时恢复目标不再是按照MySQL正常启动流程重建实例，而是在只读条件下识别残留证据、约束候选数据并尽可能形成可审计的恢复结果。

磁盘级灾难恢复面临三个基本矛盾。第一，存储层残留通常缺少完整的目录、文件名和元数据，不能简单依赖路径发现。第二，InnoDB页中的二进制内容同时包含页头、记录链、事务字段、变长字段和校验信息，局部损坏会造成字段边界和记录身份的不确定。第三，恢复工具容易把“发现候选页”“解析出若干行”或“生成内部可导入判定”误认为成功恢复，从而产生结构化误报。对于灾难恢复而言，错误导出比少导出更危险，因为错误数据可能被重新导入并进一步污染业务系统。

本文以MySQL Recovery Pro的现有实现和验证材料为理论支撑，研究MySQL 9.0无备份条件下的磁盘级恢复逻辑。研究重点是恢复过程的证据约束、版本适配、页面与记录解析、文件系统残留利用以及结构化结果的严格判定，而不是设计新的MySQL内核功能或提供生产运维操作手册。

### 1.2 研究问题

本文主要研究以下问题：

1. 如何在缺少正常目录和完整元数据的情况下定位具有InnoDB特征的物理页；
2. 如何兼容MySQL 5.6/5.7的FRM路径与MySQL 8.0、8.4、9.0的SDI路径；
3. 如何利用B+Tree、记录链、Undo、Redo和BLOB等信息恢复具有身份约束的数据；
4. 如何处理JSON、压缩页、加密页以及ext4/XFS文件系统残留；
5. 如何通过严格ground-truth comparator区分结构化恢复、取证证据和明确拒绝；
6. 如何在实验统计中避免重复页、历史残留、延迟分配和clean shutdown等因素造成虚假的恢复率。

### 1.3 研究范围与限制

本文研究对象为磁盘上仍然存在的普通文件、表空间页、日志片段和文件系统残留。工具不修改MySQL内核，不以启动损坏实例或绕过数据库一致性检查为前提，也不将物理损毁的数据凭空重建。若目标数据对应的物理扇区已被完全覆盖、清零或不可读取，则恢复逻辑没有可利用的事实依据，无法保证恢复。

本文中的结果需要区分四类：源码和算法能力、单元及模糊测试结果、合成fixture或文件级验证结果，以及真实MySQL灾难实验结果。未完成或尚未封存的Phase 5、Phase 6、Podman和真实设备实验不作为本文既成结论。历史实验中的恢复行数、百分比和设备规模仅在明确绑定数据集、删除时序、flush策略、统计口径和去重规则时具有参考意义。

## 2 相关技术基础

### 2.1 MySQL版本演进与元数据差异

MySQL 5.6和5.7的传统InnoDB恢复通常需要结合数据库目录中的FRM文件。FRM保存表定义相关信息，包括字段类型、字段长度、字符集、排序规则、枚举或集合元素以及索引定义。恢复工具通过FRMParser提取ColumnDef和KeyDef等结构，建立字段顺序、物理长度和解码方式。FRM路径的优点是结构信息相对直接，缺点是FRM文件可能已经被删除、截断或与残留表空间不匹配。

MySQL 8.0开始采用数据字典机制，表结构和索引信息通过Serialized Dictionary Information（SDI）等形式与InnoDB表空间关联。MySQL 8.0以后不再以5.7时代的FRM作为主要表定义载体，恢复工具需要从表空间中的SDI页识别和提取JSON形式的字典信息。项目实现了SDI候选识别、压缩数据解压、JSON解析和schema构建，但对损坏SDI采用fail-closed策略：压缩流未完整结束、存在尾随输入、声明长度与实际长度不一致、压缩比异常或JSON不可解析时，不能直接授权结构化输出。

MySQL 9.0与MySQL 8.0在总体数据字典方向上具有连续性，但灾难恢复逻辑仍需关注版本差异。MySQL 8.0及以后使用独立的数据字典表空间体系，并引入原子DDL日志以保证DDL相关元数据操作的崩溃一致性；MySQL 9.0在此基础上继续演进存储引擎和恢复机制。对于加密部署，8.0及以后还涉及加密页和加密双写页的处理。工具可以识别部分加密候选并提供离线密钥解析或受限解密接口，但候选识别不等于原生TDE兼容性证明，也不能据此声称已完成加密双写页重放或torn page修复。

因此，版本检测在恢复流程中既用于选择元数据路径，也用于限制结论范围。MySQL 5.6/5.7优先验证FRM；MySQL 8.0/8.4/9.0优先验证SDI和表空间字典。版本标签、schema来源、可信度和证据链必须随恢复结果保存，不能只在报告中给出一个未经验证的版本名称。

### 2.2 InnoDB物理页

InnoDB表空间由固定逻辑页组成，项目默认页大小为16 KB，同时兼容4 KB、8 KB、32 KB和64 KB配置。一个典型页包含FIL Header、页主体和FIL Trailer。页头记录页类型、页号、表空间标识、LSN以及可能的前后页关系；页尾保存校验相关信息。INDEX页还包含页目录、页级别、INFIMUM和SUPREMUM伪记录以及用户记录链。

页识别不能只依赖单一魔数。PageScanner结合页类型、页号、space ID、LSN、边界、校验和、压缩状态及上下文进行候选判断。对于被破坏的页，HeuristicScanner可以根据文件偏移、页号和内容特征生成启发式结果，但该结果只表示“可能存在页面证据”，不表示该页中的每个记录都可靠。页扫描结果还需要经过页身份校验、表空间归属校验和后续记录完整性验证。

压缩表空间还涉及FSP Header中的压缩元数据。恢复逻辑必须检查指针和可用字节数、逻辑页大小、物理页大小及zip_ssize边界，拒绝异常压缩参数和超出输入范围的读取。仅识别出压缩页类型，不能直接把压缩内容作为结构化行输出；压缩解码成功后仍需验证页身份、记录边界、字段值和完整性。

### 2.3 B+Tree、记录与字段解码

InnoDB索引组织为B+Tree。叶子页包含用户记录，内部节点包含指向子页的引用。恢复过程首先筛选INDEX页，再根据页级别、页号、索引标识和链指针建立候选树。对叶子页，解析器从INFIMUM记录附近开始，沿记录的next指针遍历用户记录，并通过页目录和记录边界避免把随机字节解释成完整记录。

记录解析涉及NULL bitmap、变长字段长度、字段物理顺序、隐藏事务字段以及行格式差异。对于DYNAMIC等行格式，字段存储位置必须以当前解析器和真实页面布局为准，不能机械套用某一历史描述。字符集会影响可变长度字段的最大字节数，ENUM、SET、BIT、DECIMAL、FLOAT、DOUBLE、日期时间、二进制字段和JSON也具有不同的编码规则。字段解码只有在schema来源可信、边界一致、类型约束成立且记录身份可验证时，才可进入结构化候选集合。

BLOB字段通常只在聚簇记录中保留部分内容和外部指针。BlobChainRebuilder通过页号、偏移、长度和链指针尝试重建外部页链，但链断裂、循环、跨表空间或长度不一致时，不能把局部片段当作完整字段。二级索引恢复同样不能直接等同于完整行；SecondaryIndexRescuer需要根据主键合并候选、检测冲突并记录缺失字段和完整性状态。

### 2.4 Undo与Redo证据

Undo记录可能包含事务标识、表标识、主键、旧字段值和前一版本指针。沿Undo链回溯可用于分析UPDATE、DELETE以及历史版本，但链条可能受到页面损坏、purge、截断、指针冲突和循环的影响。项目将Undo结果分为COMPLETE、BROKEN_PAGE、INVALID_RECORD、POINTER_CONFLICT、CYCLE、TRUNCATED和HEURISTIC等状态。只有链完整、身份一致且字段解析成功时，历史值才具有较高可信度。

Redo处理的安全边界更为严格。当前实现能够建立redo inventory，审计部分MLOG类型，生成受页身份、space ID、校验和及前置条件约束的patch plan，并通过显式replay gate控制是否允许应用补丁。但该机制不是完整复刻MySQL 9.0崩溃恢复：页面容器可能缺少space ID维度，部分MLOG类型的应用语义被简化，尚未完整覆盖mini-transaction、压缩页、BLOB、事务和Undo关联、redo block校验及LSN wraparound。因此，Redo输出应表述为受约束的日志证据和页面patch原型，而非可直接启动的datadir或经过MySQL 9.0导入验证的.ibd。

### 2.5 ext4与XFS文件系统残留

文件系统层恢复为数据库文件发现提供了补充证据。ext4场景可利用inode、块组、空闲块和已解除链接文件的残留；XFS场景则可利用allocation group、inode结构、free inode B+Tree、extent B+Tree和unlinked inode chain。XFS还可能需要处理稀疏inode、延迟分配和不同的extent记录形态。文件系统解析器输出的通常是文件或数据块候选，仍须交给InnoDB页和记录层进行二次验证。

文件系统恢复率受flush时序、延迟分配、删除方式、文件覆盖、历史残留和重复页影响。clean shutdown可能使Undo被purge，导致Undo实验出现零行；XFS延迟分配则可能使最近写入的数据并未在删除前实际落盘。恢复行数超过目标行数也可能表示混入历史记录或重复记录，不能解释为恢复质量超过100%。因此评价指标应同时报告目标行数、原始候选行数、去重行数、正确解析行数、误报数、漏报数、precision和recall。

## 3 恢复算法设计

### 3.1 分层恢复总体流程

本文采用“发现—识别—约束—解析—验证—输出”的分层流程：

1. **输入隔离。** 对待分析文件建立只读输入清单、哈希和来源记录，禁止恢复过程覆盖原始输入。
2. **版本与格式识别。** 根据显式用户信息、FRM、SDI、表空间页及页面特征建立版本候选，并保存证据来源。
3. **文件和页发现。** 在允许的文件范围内扫描表空间、日志和文件系统残留，识别页边界、页类型和候选文件。
4. **页身份约束。** 验证页号、space ID、页类型、LSN、表空间边界、压缩或加密状态以及可用校验信息。
5. **结构解析。** 解析FSP、INDEX页、B+Tree节点、记录链、字段长度、NULL bitmap、事务字段和BLOB链。
6. **一致性验证。** 检查主键、记录重复、页链、索引归属、schema来源、字段类型、完整性状态和跨页引用。
7. **严格比较。** 与canonical ground truth进行完整schema和完整行集比较；没有ground truth时不能把候选行宣称为精确恢复。
8. **分级输出。** 只有满足结构化门槛的记录才能进入结构化结果；不满足门槛的内容进入证据结果或被拒绝。

该流程的核心不是尽可能扩大候选集合，而是在每一层减少不受约束的解释。页面发现可以容忍一定启发式，但结构化导出必须以零结构化误报为目标。

### 3.2 页候选识别与坏块检测伪代码

```text
scan(input):
    manifest <- record_input_identity(input)
    pages <- locate_page_candidates(input)

    for candidate in pages:
        if not within_declared_boundary(candidate):
            reject(candidate, "boundary")
            continue
        page <- read(candidate)
        identity <- validate_page_identity(page)
        if identity.failed:
            record_evidence(candidate, identity.reason)
            continue
        classify(page)

    return pages
```

启发式扫描器可以依据页类型和页号生成结果，检测页头、记录头、页目录和页尾的异常，并报告坏块原因。但任何heuristic结果都必须携带offset、page number、space ID、来源文件、验证状态和置信度。对于无法确认页身份的内容，只能作为原始证据，不得直接进入CSV或SQL。

### 3.3 Schema可信度与字段解析

schema来源按可信度分层。可信FRM或完整SDI可以提供字段名、顺序、类型、长度、字符集和索引信息；部分解析或疑似损坏的schema只能用于候选解释，不能授权结构化输出。若schema缺列、列顺序不确定、存在未解析列或无法确定存储类型，则恢复结果转入证据或拒绝状态。

字段解析可抽象为以下过程：

```text
parse_record(page, record, schema):
    if not schema.is_complete:
        return EVIDENCE_ONLY
    boundaries <- decode_record_header(record)
    if boundaries.invalid:
        return EVIDENCE_ONLY
    values <- decode_by_physical_layout(record, schema, boundaries)
    if values.invalid:
        return EVIDENCE_ONLY
    if not primary_key_valid(values):
        return REJECT
    return STRUCTURED(values)
```

对于JSON，必须验证长度、二进制JSON结构和字符编码，不能仅凭可打印字符串判定字段正确。对于ENUM和SET，应验证索引或位图与schema元素集合的对应关系。对于二进制和BLOB，应保留原始字节并采用语义等价的比较表示，不能用普通文本替换损失字节信息。

### 3.4 Undo、Redo与DDL证据链

Undo恢复采用有向链分析。每条边需要满足页号、偏移、表空间和记录边界约束；遇到循环、越界、断链、事务冲突或页损坏时，链完整性状态立即降级。Redo分析则先生成inventory和白名单审计结果，再依据checkpoint、LSN顺序、目标页身份和space ID生成patch plan。任何前置条件不满足时，patch只能作为审计材料，不能静默应用。

MySQL 8.0及以后存在原子DDL日志和数据字典变化，恢复工具可根据页布局或物理列数差异发现DDL variation，但这类DDLChangeDetector属于推断路径，不能证明完整的Atomic DDL时间线、默认值、隐藏列或DDL提交状态。INSTANT DDL中断场景需要同时保存旧版本和新版本schema证据，不能仅凭列数变化决定最终表结构。

### 3.5 证据分级与输出策略

本文采用四级证据语义：L1为STRUCTURED，L2为FRAGMENT，L3为STRING_ONLY，L4为RAW。L1和L2仍需经过schema、身份、字段边界、主键和完整性约束；L3和L4只进入evidence_fragments，并保留来源偏移、页面身份、提取方法和置信度。

按照已批准策略，L3置信度保持0.4，L4置信度保持0.3。该数值表示证据分级中的保守置信度，不表示真实恢复率。L3和L4永远不得生成CSV或SQL，也不得通过内部IMPORTABLE标记绕过输出策略。对于混合结构化和证据结果，若无法明确分离，采用fail-closed，禁止结构化导出。

### 3.6 严格ground-truth comparator

结构化结果只有在以下条件同时满足时才判定为PASS：

- schema字段名和顺序完全一致；
- 不存在缺失列、额外列、未解析列或schemaless列；
- 主键列存在且在两侧均唯一；
- 行数完全一致；
- 主键集合完全一致；
- 每个支持字段逐值一致；
- 不存在重复主键和重复记录；
- schema fingerprint和row-multiset fingerprint一致；
- false positive和false negative均为零；
- 恢复结果被标记为可导入，且没有evidence-only泄漏。

允许的规范化仅限表达形式不同但语义相同的情况，例如JSON对象键排序，或UNHEX表达式与对应二进制值。不能进行无条件空白裁剪，因为空格可能是CHAR、TEXT或二进制字段的真实值。比较器必须把“共同列匹配”但“额外列丢失”的结果判定为失败，避免部分字段通过造成假阳性。

## 4 实验与结果分析

### 4.1 实验设计

实验分为四个层次。第一层为解析器单元测试和模糊测试，验证边界检查、长度限制、字节序、记录链、SDI解压、BLOB指针和字段解码不会因异常输入崩溃或越界。第二层为合成fixture测试，验证页面损坏、链断裂、压缩元数据异常、L3/L4输出隔离和结构化比较策略。第三层为文件级验证，要求输入manifest、文件哈希和场景定义完整，禁止自动发现未声明输入。第四层为真实Podman和文件系统灾难实验，计划覆盖MySQL 5.6、5.7、8.0、8.4和9.0，每个版本使用不少于100000行并覆盖该版本支持的字段类型，文件系统标签包括ext4和xfs2，其中xfs2在报告中规范化记录为xfs，而不声称存在独立xfs2后端。

真实实验还应区分Podman fixture捕获确认和文件系统操作确认。创建镜像、attach loop、格式化、挂载、原始写入、卸载、detach和清理不属于普通单元或文件级验证范围，必须拥有独立确认边界。本文不把尚未完成封存的真实实验写成已验证结论。

### 4.2 已完成验证与其解释

当前可确认的验证包括：严格ground-truth comparator回归测试通过，覆盖共同列误通过、缺失列、列重排、额外列、重复主键、有意义的空白差异以及L3/L4 CSV/SQL泄漏；此前一轮隔离Phase 5报告记录了baseline和ASan测试通过，但该报告同时明确真实MySQL fixture因镜像拉取限制未运行，因此只能证明对应测试和策略，不等同真实灾难恢复率。后续LSan构建曾因heuristic_scanner.hpp缺少algorithm导致std::sort编译失败，加入头文件后，sanitized构建继续完成test_heuristic_scanner及后续多个目标；该结果证明编译缺陷被修复，但不能替代完整重新封存的Phase 5报告。

历史实验曾报告XFS在特定设备、flush时序和数据集下接近98.77%的目标数据恢复率，也曾报告ext4场景接近完整恢复。但这些数字受延迟分配、落盘时序、删除方式、历史残留和统计口径影响，不能概括为普遍保证。某些历史输出中原始恢复行数与正确解析行数存在差异，说明“发现候选记录”与“正确解析记录”必须分开统计。行数超过目标值时，还必须分析重复页和历史记录混入，而不是直接提高恢复率。

### 4.3 评价指标

设目标ground truth记录集合为G，结构化恢复集合为R，正确匹配集合为C，则：

- precision = |C| / |R|；
- recall = |C| / |G|；
- false positive = |R - G|；
- false negative = |G - R|。

对于严格结构化PASS，要求schema、主键集合、字段值、重复性和fingerprint全部一致，并且false positive与false negative均为零。对于证据类结果，不使用结构化precision替代证据可信度，而报告证据数量、来源偏移、页身份状态、解析级别和人工复核状态。L3=0.4和L4=0.3仅属于证据置信度，不能转化为recall或precision。

### 4.4 复杂场景分析

**压缩页。** 压缩页必须经过物理页大小、zip_ssize、压缩流边界和解码长度验证。压缩识别成功不代表记录可读；若解压结果无法形成合法页和合法记录，结果只能进入证据或拒绝。

**加密页。** TDE候选扫描可以发现疑似加密页，离线keyring解析器可在满足格式和密钥条件时提供解密接口，但尚不能据此证明原生MySQL TDE、keyring、加密双写页和torn page修复的完整兼容性。密钥未知或身份校验失败时必须拒绝结构化导出。

**Undo/Flashback。** clean shutdown可能触发Undo purge，导致实验中没有可用旧版本。该现象不能单独解释为解析器错误。可靠验证应采用crash-preserved或等价的非正常终止场景，并记录事务提交状态、purge状态、链完整性和版本边界。

**Redo。** 受限patch plan可帮助分析页变化，但它不等于MySQL 9.0完整崩溃恢复。测试结果应报告inventory、被接受和拒绝的MLOG类型、页身份检查、LSN范围以及未覆盖的语义。

**分区与共享表空间。** 分区恢复可以发现分区文件、space ID和index ID候选，但不能仅凭物理页重建完整分区定义。共享表空间中的顺序page bundle也不一定保持原始物理offset，因此页面聚合后的连续性不能作为原始布局证明。

## 5 总结与展望

### 5.1 研究结论

本文构建了面向MySQL 9.0无备份环境的磁盘级灾难恢复逻辑框架。该框架以版本适配为入口，以FRM和SDI为schema来源，以InnoDB物理页、B+Tree、记录链、Undo、Redo和BLOB为核心证据，以ext4/XFS残留为文件发现补充，并以严格ground-truth comparator作为结构化输出的最终门槛。

研究的关键原则是：候选发现可以启发式，结构化导出必须确定性；证据可以保留不完整信息，但必须标注来源和置信度；分类成功不能替代字段逐值比较；恢复行数不能替代precision和recall；内部IMPORTABLE不能替代真实数据库导入验证。L3和L4分别保持0.4和0.3的证据置信度，仅用于取证，永远不得生成CSV或SQL。物理扇区完全损毁时不存在可验证的恢复依据，任何工具都不能突破这一边界。

### 5.2 局限性

当前实现仍存在若干边界。MySQL 8.0、8.4和9.0之间的细粒度差异尚不能仅由SDI逻辑完整区分；Redo重放尚未完整复刻MySQL 9.0所有mini-transaction和页面语义；原生TDE、keyring、加密双写页和torn page恢复尚缺少充分真实验证；共享表空间、分区定义、延迟分配和历史残留会影响文件系统实验统计；clean shutdown下Undo purge会限制Flashback验证。上述限制决定了工具应被定位为应急抢救和取证辅助工具，而非可替代数据库备份体系的通用恢复产品。

### 5.3 展望

后续工作包括：建立digest固定且可复现的多版本Podman fixture；完成5.6至9.0不少于100000行的版本化字段库存验证；开展crash-preserved Undo和Redo实验；验证Atomic DDL和INSTANT DDL中断边界；完善加密页和加密双写页的原生格式验证；改进共享表空间和分区表的身份约束；在ext4与XFS实验中严格排除历史残留和重复数据；建立可审计的文件级和真实灾难级报告封存流程。

无论后续算法如何改进，正规备份、binlog、远程副本、容灾切换和定期恢复演练仍是生产数据库可靠性的基础。磁盘级恢复只能在这些机制失效或不可用时尽可能抢救残留数据，不能替代预防性数据保护体系。

## 参考文献

[1] Oracle Corporation. MySQL 9.0 Reference Manual[M]. Oracle Corporation, 2024.

[2] Oracle Corporation. MySQL 9.0 Reference Manual: The InnoDB Storage Engine[M]. Oracle Corporation, 2024.

[3] Oracle Corporation. MySQL 9.0 Reference Manual: InnoDB Data Dictionary[M]. Oracle Corporation, 2024.

[4] Oracle Corporation. MySQL 9.0 Reference Manual: Atomic Data Definition Statement Support[M]. Oracle Corporation, 2024.

[5] Oracle Corporation. MySQL 9.0 Reference Manual: InnoDB Tablespace Encryption[M]. Oracle Corporation, 2024.

[6] Oracle Corporation. MySQL 8.0 Reference Manual: Serialized Dictionary Information[M]. Oracle Corporation, 2024.

[7] Oracle Corporation. MySQL 5.7 Reference Manual: The .frm File[M]. Oracle Corporation, 2020.

[8] GRAY J, REUTER A. Transaction Processing: Concepts and Techniques[M]. San Francisco: Morgan Kaufmann, 1993.

[9] ELNASHAAR A, et al. Database recovery techniques: a survey[J]. ACM Computing Surveys, 2023, 55(8): 1-38.

[10] 刘强, 王磊, 张建勋. 数据库故障恢复技术研究综述[J]. 计算机科学, 2021, 48(增刊): 1-8.

[11] 王珏, 陈志刚. 面向存储介质故障的数据库数据恢复方法研究[J]. 计算机工程与应用, 2020, 56(18): 25-32.

[12] ROSENTHAL D S H, et al. The future of data storage: forensic recovery and integrity verification[J]. Digital Investigation, 2022, 40: 301-312.

[13] CARD R, TS'O T, TWEEDIE S. Design and implementation of the second extended filesystem[C]//Proceedings of the First Dutch International Symposium on Linux. Amsterdam: Linux International, 1994: 1-6.

[14] SWEENIE D. Scalability in the XFS file system[C]//Proceedings of the USENIX Annual Technical Conference. Berkeley: USENIX Association, 1996: 1-12.

[15] ISO/IEC 9075-1:2023. Information technology—Database languages—SQL—Part 1: Framework[S]. Geneva: International Organization for Standardization, 2023.
