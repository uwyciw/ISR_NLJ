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

#ifndef _ISR_NLJ_H_
#define _ISR_NLJ_H_

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "stm32h5xx.h"

/**
 * @brief 初始化ISR非局部跳转模块，将PendSV优先级设为最低（0xFF）。
 * @retval 无。
 */
void ISR_NLJ_Init(void);

/**
 * @brief 设置非局部跳转的目标函数。
 * @param target 跳转目标函数指针，该函数将在PendSV异常返回后被激活执行。
 * @retval 无。
 */
void ISR_NLJ_SetJumpTarget(void(*target)(void));

/**
 * @brief 触发PendSV挂起，调度一次非局部跳转。
 * @details 向ICSR写入PENDSVSET位，使PendSV异常在最低优先级上触发，
 *          PendSV_Handler将在异常返回时合成栈帧并跳转到目标函数。
 * @retval 无。
 */
void ISR_NLJ_Schedule(void);

#endif // _ISR_NLJ_H_