/**
 * @file zero_cross_relay.h
 * @brief 过零检测固态继电器组件头文件（ESP-IDF PCNT + CPU中断版本）
 * 
 * 功能：
 * - 使用PCNT硬件计数器监测AC电源的过零点（GPIO3输入）
 * - 计数范围：0-20，自动循环清零
 * - Watch Point：计数到10时拉低GPIO4，计数到20时拉高GPIO4
 * - 提供中断触发计数和频率统计
 * - 使用PCNT Watch Point中断实现精确相位控制
 * 
 * 硬件连接：
 * - GPIO3: 过零检测输入（上升沿计数，内部上拉）
 * - GPIO4: 固态继电器输出（初始高电平，计数10时拉低，计数20时拉高）
 * 
 * @note 此实现仅兼容ESP-IDF框架（ESP32-C6）
 * 
 * @author chinawrj@gmail.com
 * @date 2025-10-11
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

// ESP-IDF PCNT (Pulse Counter) API
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"    // PCNT driver for edge counting
#include "driver/gptimer.h"      // GPTimer for precise delay
#include "esp_timer.h"

namespace esphome {
namespace zero_cross_relay {

/**
 * @class ZeroCrossRelayComponent
 * @brief 过零检测固态继电器组件类
 */
class ZeroCrossRelayComponent : public Component {
 public:
  /**
   * @brief 设置过零检测输入引脚
   * @param pin GPIO引脚对象指针
   */
  void set_zero_cross_pin(InternalGPIOPin *pin) { zero_cross_pin_ = pin; }

  /**
   * @brief 设置继电器输出引脚
   * @param pin GPIO引脚对象指针
   */
  void set_relay_output_pin(InternalGPIOPin *pin) { relay_output_pin_ = pin; }

  /**
   * @brief 组件初始化（setup阶段）
   * 
   * 配置GPIO引脚并注册中断服务例程
   */
  void setup() override;

  /**
   * @brief 组件主循环（loop阶段）
   * 
   * 用于周期性任务（如频率计算）
   */
  void loop() override;

  /**
   * @brief 获取组件优先级
   * @return float 优先级（较高值优先初始化）
   */
  float get_setup_priority() const override { return setup_priority::IO; }

  /**
   * @brief 输出组件配置信息到日志
   */
  void dump_config() override;

 protected:
  InternalGPIOPin *zero_cross_pin_{nullptr};   ///< 过零检测输入引脚
  InternalGPIOPin *relay_output_pin_{nullptr}; ///< 继电器输出引脚

  // PCNT (Pulse Counter) 相关
  pcnt_unit_handle_t pcnt_unit_{nullptr};      ///< PCNT单元句柄（计数0-20，自动循环）
  pcnt_channel_handle_t pcnt_channel_{nullptr}; ///< PCNT通道句柄（GPIO3上升沿计数）
  
  // GPTimer (Hardware Timer) 相关 - 用于延时控制
  gptimer_handle_t delay_timer_{nullptr};      ///< GPTimer句柄（用于2000us延时）
  
  volatile uint32_t trigger_count_{0};         ///< PCNT watch point触发计数器（10和20的总次数）
  volatile uint32_t cycle_count_{0};           ///< 完整周期计数器（20次/周期）
  volatile uint32_t last_cycle_time_{0};       ///< 上次周期完成时间戳（us）
  float estimated_frequency_{0.0f};            ///< 估算AC频率（Hz）- 基于20次计数周期
  
  // GPIO控制状态（用于定时器中断中判断应该设置高电平还是低电平）
  volatile int pending_gpio_level_{-1};        ///< 待设置的GPIO电平（0=LOW, 1=HIGH, -1=无待处理）
  
  gpio_num_t zero_cross_gpio_num_;             ///< Zero-cross detection GPIO number (ESP-IDF format)
  gpio_num_t relay_output_gpio_num_;           ///< Relay output GPIO number (ESP-IDF format)

  /**
   * @brief PCNT Watch Point中断回调函数（ISR上下文）
   * 
   * 当PCNT计数到达Watch Point（10或20）时由硬件触发
   * 不直接控制GPIO，而是启动硬件定时器延时2000us
   * 
   * @param unit PCNT单元句柄
   * @param edata Watch Point事件数据（包含触发值）
   * @param user_ctx 用户上下文指针（this指针）
   * @return bool 是否需要唤醒更高优先级任务
   */
  static bool IRAM_ATTR pcnt_on_reach_callback(pcnt_unit_handle_t unit,
                                                const pcnt_watch_event_data_t *edata,
                                                void *user_ctx);

  /**
   * @brief GPTimer报警中断回调函数（ISR上下文）
   * 
   * 在PCNT中断触发后延时2000us执行
   * 根据pending_gpio_level_的值控制GPIO4电平
   * 
   * @param timer GPTimer句柄
   * @param edata 报警事件数据
   * @param user_ctx 用户上下文指针（this指针）
   * @return bool 是否需要唤醒更高优先级任务
   */
  static bool IRAM_ATTR timer_alarm_callback(gptimer_handle_t timer,
                                              const gptimer_alarm_event_data_t *edata,
                                              void *user_ctx);
};

}  // namespace zero_cross_relay
}  // namespace esphome
