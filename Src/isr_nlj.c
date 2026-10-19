/**
  ******************************************************************************
    * @file isr_nlj.h
    * @author lx
    * @date 2026-10-17
    * @brief 在ISR返回时，实现非局部跳转。
   =============================================================================
                        #####  #####
   =============================================================================


  ******************************************************************************
    * @attention
    *
  ******************************************************************************
**/

/* Includes ------------------------------------------------------------------*/
#include "isr_nlj.h"

/*============================================================================*
 * 编译器适配层（IAR / Keil AC6 / Keil AC5 / GCC）
 *
 * NLJ_NAKED —— 裸函数属性：不生成任何 prologue/epilogue。PendSV_Handler、
 *              NMI_Handler、ISR_NLJ_Return 对栈指针做字节级精确操作，
 *              编译器自动生成的栈帧会破坏"合成帧 / POP {r0,pc}"的配对
 *              关系，因此这三个函数必须为裸函数。
 *              Keil AC5 (armcc) 不支持 naked 属性，改用 __asm void 嵌入式
 *              汇编实现（天然无 prologue），见文件末尾 AC5 分支。
 *
 * NLJ_KEEP  —— 防止仅被内联汇编（而非 C 代码）引用的函数被编译器/链接器
 *              优化剔除：ISR_NLJ_Activate 与 ISR_NLJ_Return 的地址只出现在
 *              汇编字面量中，链接器可能认为它们未被引用。
 *
 * 汇编写法  —— 指令序列（PUSH/CPSID/MOVS/LSLS/STM/MVN/BX/DSB 等）对三类
 *              编译器的内联汇编器通用；32 位常量与符号地址统一使用
 *              "LDR rX,=expr" 伪指令，由各编译器汇编器自行生成字面量池
 *              （替代原 GNU as 手工 ".ALIGN/.WORD" 池写法，后者不被
 *              IAR/armasm 支持）。
 *============================================================================*/
#if defined(__ICCARM__)
    #define NLJ_NAKED     __naked
    #define NLJ_KEEP      __root
#elif defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010000)   /* Keil AC6 */
    #define NLJ_NAKED     __attribute__((naked))
    #define NLJ_KEEP      __attribute__((used))
#elif defined(__ARMCC_VERSION)                                    /* Keil AC5 */
    #define NLJ_NAKED
    #define NLJ_KEEP      __attribute__((used))
#else                                                             /* GCC */
    #define NLJ_NAKED     __attribute__((naked))
    #define NLJ_KEEP      __attribute__((used))
#endif

/* 带硬件 FPU 编译时需要清除 CONTROL.FPCA 位（见 ISR_NLJ_Return） */
#if defined(__ARM_FP) || (defined(__ARMCC_VERSION) && (__ARMCC_VERSION < 6010000) && defined(__TARGET_FPU_VFP))
    #define NLJ_HAS_FPU   1
#endif

/**
 * @brief 跳转目标函数指针，由ISR_NLJ_SetJumpTarget设置，
 *        在ISR_NLJ_Activate中被调用。
 */
static volatile void(*gTarget)(void) = NULL;

/**
 * @brief 激活函数，作为合成栈帧的PC值，异常返回后硬件"返回"到此执行。
 * @note 非静态：其地址被 PendSV_Handler 的内联汇编以符号形式引用，
 *       对汇编器必须是可见的全局链接符号。
 */
static NLJ_KEEP void ISR_NLJ_Activate(void);

/**
 * @brief 任务返回蹦床函数，当目标函数return后经LR跳转至此，
 *        随后挂起NMI以恢复原始异常现场。
 * @note 非静态：理由同 ISR_NLJ_Activate。
 */
static NLJ_KEEP void ISR_NLJ_Return(void);

/**
 * @brief 初始化ISR非局部跳转模块，将PendSV优先级设为最低（0xFF）。
 * @retval 无。
 */
void ISR_NLJ_Init(void)
{
    NVIC_SetPriority(PendSV_IRQn, (1UL << __NVIC_PRIO_BITS) - 1UL);
}

/**
 * @brief 设置非局部跳转的目标函数。
 * @param target 跳转目标函数指针，该函数将在PendSV异常返回后被激活执行。
 * @retval 无。
 */
void ISR_NLJ_SetJumpTarget(void(*target)(void))
{
    gTarget = target;
}

/**
 * @brief 触发PendSV挂起，调度一次非局部跳转。
 * @details 向ICSR写入PENDSVSET位，使PendSV异常在最低优先级上触发，
 *          PendSV_Handler将在异常返回时合成栈帧并跳转到目标函数。
 * @retval 无。
 */
void ISR_NLJ_Schedule(void)
{
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

#if !(defined(__ARMCC_VERSION) && (__ARMCC_VERSION < 6010000))   /* IAR / AC6 / GCC */

/**
 * @brief PendSV中断服务程序，非局部跳转核心实现。
 * @details 在PendSV入口处：
 *          1. 保存r0和原始EXC_RETURN到栈上；
 *          2. 关闭全局中断（PRIMASK=1），保证合成栈帧过程原子；
 *          3. 清除PendSV挂起位，防止重复触发；
 *          4. 在MSP上合成8字基本异常栈帧（LR=ISR_NLJ_Return,
 *             PC=ISR_NLJ_Activate, xPSR=0x01000000）；
 *          5. 以EXC_RETURN=0xFFFFFFF9触发异常返回，
 *             硬件弹出合成帧，"返回"到ISR_NLJ_Activate执行。
 * @retval 无。
 */
NLJ_NAKED void PendSV_Handler(void)
{
    __asm volatile (
    /* 保存 r0 和进入 ISR 时的 LR（即原始 EXC_RETURN，如 0xFFFFFFFD）：
     * - EXC_RETURN 留给 NMI_Handler 伪造异常返回使用；
     * - 压入 2 个字使栈保持 8 字节对齐；
     * - r0 将在 NMI_Handler 里由 POP {r0,pc} 恢复。 */
    "  PUSH    {r0,lr}                  \n"

        /* 关全局中断（PRIMASK=1）：保证合成栈帧的过程原子，
         * 此中断状态一直持续到 ISR_NLJ_Activate 中重新打开 */
        "  CPSID   i                    \n"

        /* 向 ICSR 写 1<<27（PENDSVCLR）：清除 QK_sched() 置起的 PendSV
         * 挂起位，防止本次激活完成后 PendSV 再次触发 */
        "  LDR     r2,=0xE000ED04       \n"
        "  MOVS    r1,#1                \n"
        "  LSLS    r1,r1,#27            \n"
        "  STR     r1,[r2]              \n"

        /* 准备合成栈帧的三个关键值：
         * r3 = 0x01000000 —— xPSR，bit24(T)=1，表示 Thumb 状态；
         * r2 = ISR_NLJ_Activate 地址（清 bit0：Thumb 函数符号 bit0 为 1，
         *      放入栈帧 PC 槽需为偶数）；
         * r1 = ISR_NLJ_Return 地址 —— 将放入 LR 槽，任务 return 即到此。 */
        "  MOVS    r3,#1                \n"
        "  LSLS    r3,r3,#24            \n"
        "  LDR     r2,=ISR_NLJ_Activate \n"
        "  BIC     r2,r2,#1             \n"
        "  LDR     r1,=ISR_NLJ_Return   \n"

        /* 在 MSP 上预留 8 字（32 字节）基本异常栈帧，布局依次为：
         * r0, r1, r2, r3, r12, lr, pc, xpsr。
         * 只需初始化 lr/pc/xpsr 三个槽；其余槽保留垃圾值即可——
         * 原始上下文的这些寄存器已保存在最初被打断现场的帧里。 */
        "  SUB     sp,sp,#(8*4)         \n"
        "  ADD     r0,sp,#(5*4)         \n" /* r0 -> 栈帧第 5 个字（LR 槽） */
        "  STM     r0!,{r1-r3}          \n" /* LR=ISR_NLJ_Return, PC=ISR_NLJ_Activate, xPSR=0x01000000 */

        /* r0 = ~6 = 0xFFFFFFF9，即 EXC_RETURN：
         * 返回 Thread 模式、使用 MSP、无 FPU 扩展帧——
         * 恰好匹配刚合成的 8 字基本帧 */
        "  MOVS    r0,#6                \n"
        "  MVNS    r0,r0                \n"
        "  DSB                          \n" /* 数据同步屏障：确保 ICSR 写入生效后再返回 */
        "  BX      r0                   \n" /* 以 EXC_RETURN 触发异常返回：硬件弹出合成帧，
                                               "返回"到 ISR_NLJ_Activate（LR=ISR_NLJ_Return，
                                               运行于 Handler 模式，本函数隐式结束） */
        );
}

/**
 * @brief NMI中断服务程序，负责恢复原始异常现场。
 * @details NMI由ISR_NLJ_Return挂起，因其不可屏蔽保证立即触发：
 *          1. 跳过NMI入口自动压栈的合成帧（32字节）；
 *          2. 打开全局中断（PRIMASK=0）；
 *          3. 弹出PendSV保存的{r0,lr}，将原始EXC_RETURN写入PC，
 *             触发真正的异常返回，恢复被打断的代码继续执行。
 * @retval 无。
 */
NLJ_NAKED void NMI_Handler(void)
{
    __asm volatile (
    "  ADD     sp,sp,#(8*4)         \n" /* 跳过 NMI 入口自动压栈的合成帧（死数据） */
        "  CPSIE   i                    \n" /* NMI 入口硬件会置 PRIMASK=1，此处必须重新
                                               打开全局中断，保证恢复现场后中断使能 */
        "  POP     {r0,pc}              \n" /* 弹出 PendSV 保存的 {r0,lr}：
                                               - r0 恢复原值；
                                               - pc 载入原始 EXC_RETURN 值——向 pc 写入
                                                 EXC_RETURN 会触发真正的异常返回，
                                                 硬件从原栈弹出真实现场帧，
                                                 被打断的代码从此处无缝继续执行 */
        );
}

/**
 * @brief 任务返回蹦床函数，当目标函数return后经LR跳转至此。
 * @details 若编译目标带FPU，先清除CONTROL.FPCA位（bit2），
 *          声明当前上下文不再使用FP寄存器，使NMI入口只压8字基本帧；
 *          随后向ICSR写入NMIPENDSET位挂起NMI，NMI不可屏蔽保证立即触发，
 *          然后自旋等待NMI抢占。
 * @retval 无。
 */
static NLJ_KEEP NLJ_NAKED void ISR_NLJ_Return(void)
{
    __asm volatile (
#ifdef NLJ_HAS_FPU
    /* 仅当编译目标带 FPU 时需要：清除 CONTROL 寄存器的 FPCA 位（bit2），
     * 声明当前上下文不再使用 FP 寄存器。这样随后 NMI 异常入口只会压栈
     * 8 字基本帧，与 NMI_Handler 中固定跳过的 32 字节相匹配 */
    "  MRS     r0,CONTROL           \n"
        "  BIC     r0,r0,#4             \n"
        "  MSR     CONTROL,r0           \n"
        "  ISB                          \n"
#endif

        /* 向 ICSR 写 1<<31（NMIPENDSET）：挂起 NMI。
         * 选 NMI 的原因：NMI 不可被 PRIMASK 等任何手段屏蔽，
         * 即使全局中断处于关闭状态也保证立即触发 */
        "  LDR     r0,=0xE000ED04       \n"
        "  MOVS    r1,#1                \n"
        "  LSLS    r1,r1,#31            \n"
        "  STR     r1,[r0]              \n"

        "  B       .                    \n" /* 自旋等待：NMI 将立即抢占这条指令 */
        );
}

#else   /* Keil AC5 (armcc)：嵌入式汇编实现，天然无 prologue/epilogue */

__asm void PendSV_Handler(void)
{
    PUSH    {r0,lr}              ; 保存 r0 与原始 EXC_RETURN，供 NMI 恢复
    CPSID   i                    ; 关全局中断，保证合成栈帧过程原子
    LDR     r2,=0xE000ED04       ; ICSR 地址
    MOVS    r1,#1
    LSLS    r1,r1,#27            ; 1<<27 = PENDSVCLR，清除挂起位
    STR     r1,[r2]
    MOVS    r3,#1
    LSLS    r3,r3,#24            ; xPSR = 0x01000000，bit24(T)=1
    LDR     r2,=ISR_NLJ_Activate ; PC 槽：激活函数地址
    BIC     r2,r2,#1             ; 清 bit0，栈帧 PC 槽需为偶数
    LDR     r1,=ISR_NLJ_Return   ; LR 槽：任务返回蹦床地址
    SUB     sp,sp,#(8*4)         ; 预留 8 字基本异常栈帧
    ADD     r0,sp,#(5*4)         ; r0 -> 栈帧 LR 槽
    STM     r0!,{r1-r3}          ; LR/PC/xPSR 三槽
    MOVS    r0,#6
    MVNS    r0,r0                ; r0 = 0xFFFFFFF9 (EXC_RETURN, Handler/MSP)
    DSB
    BX      r0                   ; 触发异常返回到 ISR_NLJ_Activate
}

__asm void NMI_Handler(void)
{
    ADD     sp,sp,#(8*4)         ; 跳过 NMI 入口自动压栈的合成帧
    CPSIE   i                    ; 重开全局中断
    POP     {r0,pc}              ; 恢复 r0，PC 载入原始 EXC_RETURN
}

__asm void ISR_NLJ_Return(void)
{
#ifdef NLJ_HAS_FPU
    MRS     r0,CONTROL           ; 清 CONTROL.FPCA（bit2），使 NMI
    BIC     r0,r0,#4             ; 入口只压 8 字基本帧
    MSR     CONTROL,r0
    ISB
#endif
    LDR     r0,=0xE000ED04       ; ICSR 地址
    MOVS    r1,#1
    LSLS    r1,r1,#31            ; 1<<31 = NMIPENDSET
    STR     r1,[r0]              ; 挂起 NMI（不可屏蔽，立即触发）
    B       .                    ; 自旋等待 NMI 抢占
}

#endif   /* __ARMCC_VERSION < 6010000 */

/**
 * @brief 激活函数，作为合成栈帧的PC值，异常返回后硬件"返回"到此执行。
 * @details 将gTarget快照到局部变量以保证任务执行期间的一致性，
 *          随后打开全局中断（清PRIMASK），调用目标函数；
 *          目标函数return后经LR跳转到ISR_NLJ_Return。
 * @retval 无。
 */
static NLJ_KEEP void ISR_NLJ_Activate(void)
{
    /* 先把 volatile 的 QK_task 快照到局部变量：
     * 1) 任务执行期间 QK_task 可能被中断级代码修改，
     *    快照保证本次激活的一直是同一个任务；
     * 2) 编译器只需读取一次该 volatile 变量。 */
    void(*t)(void) = gTarget;
    /* 打开全局中断（清 PRIMASK）：任务运行期间应允许响应中断。
     * CMSIS 的 __enable_irq() 按编译器分支实现，三编译器通用，
     * 等效于原来的 "cpsie i" 内联汇编 */
    __enable_irq();
    /* 调用任务函数；任务 return 后经 LR 跳转到 ISR_NLJ_Return */
    (*t)();
}
