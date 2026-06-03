这是我的MXFP8类型的FA实现/home/ubuntu/workspace/oyhj/cutlass/examples/77_blackwell_fmha，我只需要MXFP8的flash attn fwd部分，不用管其他部分，对应cuda文件是77_blackwell_fmha.cu，现在缩放因子为1的情况下精度正常能通过验证，但是在缩放因子非1的情况下，精度错误。具体情况如下：
1. 在只有在输入seq_len=1且head_num=1时，K为任何值，精度能通过，但是当设置为>1时精度错误。
2. 在将QK都设置为1，SFQ设置为非1，精度能够通过。
3. row=0的输出计算正确。

问题可能出在UTCCP将缩放因子从SMEM搬运到TMEM的过程，第二个情况可能说明缩放因子计算时所在行是正确的，但是在行内的顺序是错误的。
但是还没进一步定位到问题，需要进一步排查。

现在我需要帮我修正这个精度问题。你可以通过在/home/ubuntu/workspace/oyhj/cutlass/build目录下使用 make 101_blackwell_fmha_mxfp8 -j来编译代码，使用/home/ubuntu/workspace/oyhj/cutlass/build/examples/77_blackwell_fmha/101_blackwell_fmha_mxfp8这个来执行代码，例如：/home/ubuntu/workspace/oyhj/cutlass/build/examples/77_blackwell_fmha/101_blackwell_fmha_mxfp8 --b=1 --h=40 --d=128 --q=1024 --k=2048，后面你可以指定输入的形状，并且可以加--verify来验证精度，在执行代码前面加REF_PRINT_DIFF=1来打印不同的数值，我给你编译运行代码的权力，我只允许你修改/home/ubuntu/workspace/oyhj/cutlass/examples/77_blackwell_fmha这部分的文件，并且这部分文件的修改权限全给你，不用问我，一律统一，但是不准动其他目录下的文件，但你可以参考，你可以阅读，比如/home/ubuntu/workspace/oyhj/temp/cutlass这是官方cutlass代码，/home/ubuntu/workspace/chenyun/archive/src/operators/op6_mxfp8_fmha这是其他目录可以参考的代码。
我需要你帮我纠正我的mxfp8的FA的精度，/home/ubuntu/workspace/oyhj/cutlass/examples/77_blackwell_fmha在这里的MXFP8的FA的基础上进行纠正，/home/ubuntu/workspace/oyhj/cutlass/examples/77_blackwell_fmha/reference这里有参考的计算，精度验证用里面的mxfp8类型的参考来验证，如果参考计算不正确，你可以纠正，。我这是MXFP8的FA，QK计算要是MXFP8，同时PV部分也必须是MXFP8计算。
现在我给你一个计划：
第一步：代码理解与问题复现
深入阅读 /home/ubuntu/workspace/oyhj/cutlass/examples/77_blackwell_fmha 目录下的相关代码，重点理解 MXFP8 FA FWD 的主计算逻辑、缩放因子处理流程以及 UTCCP 搬运机制。
查阅官方 cutlass (/home/ubuntu/workspace/oyhj/temp/cutlass) 及参考实现 (/home/ubuntu/workspace/chenyun/archive/src/operators/op6_mxfp8_fmha) 中的对应逻辑，对比找出可能的差异点。
编译并运行现有代码，确认问题的可复现性：分别在缩放因子为 1 和不为 1 的情况下验证精度，记录失败模式。
第二步：分阶段定位精度误差根源
采用逐步隔离的策略，逐一固定变量，缩小问题范围：
1.仅将 SFQ 设为非 1（SFK=1, SFV=1） → 验证精度是否通过。若通过，说明问题不在 Q 的缩放因子本身，而是与 K/V 的缩放因子配合时出错。
2.仅将 SFK 设为非 1（SFQ=1, SFV=1） → 验证精度是否通过。
3.仅将 SFV 设为非 1（SFQ=1, SFK=1） → 验证精度是否通过。
4.SFQ 与 SFK 同时为非 1（SFV=1） → 验证精度。
5.三者同时为非 1 → 完整场景验证。
每一小步通过后，再进入下一步，确保问题被精确锁定在某个或某几个缩放因子的处理上。
第三步：针对性插桩调试与修复
基于第二步的结论，在关键路径上插入调试输出（如 UTCCP 搬运前后的 SMEM/TMEM 数据、缩放因子在行内的排列顺序等），通过 REF_PRINT_DIFF=1 查看数值差异。
重点关注：缩放因子从 SMEM 搬运到 TMEM 时，行位置正确但行内顺序错乱的假设是否成立。
针对 row=0 正确但其他行错误的现象，分析是否存在索引计算或偏移量错误。
修改代码、编译、运行、验证，反复迭代直至所有测试用例精度通过。
直到精度正确为止！
一定不要修改除了/home/ubuntu/workspace/oyhj/cutlass/examples/77_blackwell_fmha这个目录下的其他目录下的文件