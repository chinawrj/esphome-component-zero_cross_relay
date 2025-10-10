/**
 * @file zero_cross_relay.h
 * @brief 过零检测固态继电器组件头文件（ESP-IDF硬件中断版本）
 * 
 * 功能：
 * - 监测AC电源的过零点（GPIO3输入）
 * - 在过零点时输出控制信号到固态继电器（GPIO4输出）
 * - 提供中断触发计数和频率统计
 * - 使用ESP-IDF原生GPIO中断API实现硬件中断
 * 
 * 硬件连接：
 * - GPIO3: 过零检测输入（高电平有效，内部上拉）
 * - GPIO4: 固态继电器输出（高电平有效）
 * 
 * @note 此实现仅兼容ESP-IDF框架
 * 
 * @author GitHub Copilot
 * @date 2025-10-10
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

// ESP-IDF GPIO interrupt API
#include "driver/gpio.h"
#include "driver/gptimer.h"      // Hardware timer for timestamp capture
#include "driver/gptimer_etm.h"  // GPTimer ETM for capture task
#include "esp_timer.h"
#include "esp_etm.h"             // Event Task Matrix for hardware capture
#include "driver/gpio_etm.h"     // GPIO ETM for edge detection

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

  volatile uint32_t trigger_count_{0};         ///< Interrupt trigger counter (all edges)
  volatile uint32_t pulse_count_{0};           ///< Complete pulse counter (rising→falling)
  volatile uint32_t last_rising_time_{0};      ///< Last rising edge timestamp (us)
  volatile uint32_t pulse_width_us_{0};        ///< Latest pulse width (us) - rising to falling edge time
  volatile uint32_t pulse_interval_us_{0};     ///< Pulse interval (us) - time between two rising edges
  float estimated_frequency_{0.0f};            ///< Estimated AC frequency (Hz) - based on pulse interval
  
  // Hardware timestamp capture for ISR latency measurement
  gptimer_handle_t gptimer_{nullptr};          ///< GPTimer handle for hardware timestamp
  esp_etm_channel_handle_t etm_channel_{nullptr};  ///< ETM channel for GPIO->Timer capture
  esp_etm_event_handle_t gpio_event_{nullptr}; ///< GPIO edge event for ETM
  esp_etm_task_handle_t timer_task_{nullptr};  ///< Timer capture task for ETM
  
  volatile uint64_t hardware_timestamp_{0};    ///< Hardware-captured timestamp (us) by ETM
  volatile uint64_t software_timestamp_{0};    ///< Software ISR entry timestamp (us)
  volatile uint32_t isr_latency_ns_{0};        ///< ISR latency in nanoseconds
  
  gpio_num_t zero_cross_gpio_num_;             ///< Zero-cross detection GPIO number (ESP-IDF format)
  gpio_num_t relay_output_gpio_num_;           ///< Relay output GPIO number (ESP-IDF format)

  /**
   * @brief ESP-IDF GPIO中断服务例程 (ISR)
   * 
   * 在GPIO3检测到上升沿时由硬件触发
   * 必须使用IRAM_ATTR标记以便在IRAM中执行
   * 
   * @param arg 传递给ISR的参数（this指针）
   */
  static void IRAM_ATTR gpio_isr_handler(void *arg);
};

}  // namespace zero_cross_relay
}  // namespace esphome
