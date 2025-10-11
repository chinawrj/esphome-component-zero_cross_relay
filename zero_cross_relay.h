/**
 * @file zero_cross_relay.h
 * @brief Zero-Cross Detection Solid State Relay Component Header (ESP-IDF PCNT + CPU Interrupt Version)
 * 
 * Features:
 * - Uses PCNT hardware counter to monitor AC power zero-crossing points (GPIO3 input)
 * - Count range: 0-20, auto-clear when reaches 20
 * - Watch Point: Pull GPIO4 LOW at count 10, pull GPIO4 HIGH at count 20
 * - Provides interrupt trigger counting and frequency statistics
 * - Uses PCNT Watch Point interrupt for precise phase control
 * 
 * Hardware Connections:
 * - GPIO3: Zero-cross detection input (rising edge count, internal pull-up)
 * - GPIO4: Solid state relay output (initial HIGH, LOW at count 10, HIGH at count 20)
 * 
 * @note This implementation is only compatible with ESP-IDF framework (ESP32-C6)
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
#include "esp_err.h"
#include "esp_timer.h"

namespace esphome {
namespace zero_cross_relay {

/**
 * @class ZeroCrossRelayComponent
 * @brief Zero-Cross Detection Solid State Relay Component Class
 */
class ZeroCrossRelayComponent : public Component {
 public:
  /**
   * @brief Set zero-cross detection input pin
   * @param pin GPIO pin object pointer
   */
  void set_zero_cross_pin(InternalGPIOPin *pin) { zero_cross_pin_ = pin; }

  /**
   * @brief Set relay output pin
   * @param pin GPIO pin object pointer
   */
  void set_relay_output_pin(InternalGPIOPin *pin) { relay_output_pin_ = pin; }

  /**
   * @brief Set duty cycle flip point (controls phase/power)
   * @param flip_point GPIO flip point (when to pull LOW), range 0-20
   *                   - 0  = 0% duty cycle (always off)
   *                   - 1  = 5% duty cycle (minimum power)
   *                   - 10 = 50% duty cycle (default, half power)
   *                   - 19 = 95% duty cycle (maximum power)
   *                   - 20 = 100% duty cycle (always on)
   * 
   * @note Lower flip point = shorter on-time = lower power
   *       Higher flip point = longer on-time = higher power
   *       Duty cycle = flip_point / 20.0
   */
  void set_duty_cycle_flip_point(int flip_point);

  /**
   * @brief Get current duty cycle flip point
   * @return int Current flip point (0-20)
   */
  int get_duty_cycle_flip_point() const { return this->duty_cycle_flip_point_; }

  /**
   * @brief Get current duty cycle percentage
   * @return float Duty cycle percentage (0.0% - 100.0%)
   */
  float get_duty_cycle_percentage() const { return (this->duty_cycle_flip_point_ / 20.0f) * 100.0f; }

  /**
   * @brief Component initialization (setup phase)
   * 
   * Configures GPIO pins and registers interrupt service routines
   */
  void setup() override;

  /**
   * @brief Component main loop (loop phase)
   * 
   * Used for periodic tasks (such as frequency calculation)
   */
  void loop() override;

  /**
   * @brief Get component priority
   * @return float Priority (higher value initializes first)
   */
  float get_setup_priority() const override { return setup_priority::IO; }

  /**
   * @brief Output component configuration information to log
   */
  void dump_config() override;

 protected:
  InternalGPIOPin *zero_cross_pin_{nullptr};   ///< Zero-cross detection input pin
  InternalGPIOPin *relay_output_pin_{nullptr}; ///< Relay output pin

  // PCNT (Pulse Counter) related
  pcnt_unit_handle_t pcnt_unit_{nullptr};      ///< PCNT unit handle (count 0-20, auto-loop)
  pcnt_channel_handle_t pcnt_channel_{nullptr}; ///< PCNT channel handle (GPIO3 rising edge count)
  
  // GPTimer (Hardware Timer) related - for delay control
  gptimer_handle_t delay_timer_{nullptr};      ///< GPTimer handle (for 2000us delay)
  
  volatile uint32_t trigger_count_{0};         ///< PCNT watch point trigger counter (total count of flip point and 20)
  volatile uint32_t cycle_count_{0};           ///< Complete cycle counter (20 counts per cycle)
  volatile uint32_t last_cycle_time_{0};       ///< Last cycle completion timestamp (us)
  float estimated_frequency_{0.0f};            ///< Estimated AC frequency (Hz) - based on 20-count cycle
  
  // GPIO control state (used in timer interrupt to determine HIGH or LOW level)
  volatile int pending_gpio_level_{-1};        ///< Pending GPIO level to set (0=LOW, 1=HIGH, -1=none)
  
  // Duty cycle control (configurable flip point, range: 0-20)
  volatile int duty_cycle_flip_point_{10};     ///< GPIO flip point (when to pull LOW), range 0-20, default 10 (50% duty)
  volatile int pending_duty_cycle_flip_point_{-1};  ///< Pending flip point request (0-20, -1=none)
  volatile esp_err_t last_watch_point_update_err_{ESP_OK}; ///< Last watch point update result
  volatile bool watch_point_update_event_{false}; ///< Flag indicating watch point update result pending for log output
  
  gpio_num_t zero_cross_gpio_num_;             ///< Zero-cross detection GPIO number (ESP-IDF format)
  gpio_num_t relay_output_gpio_num_;           ///< Relay output GPIO number (ESP-IDF format)

  /**
   * @brief PCNT Watch Point interrupt callback function (ISR context)
   * 
   * Triggered by hardware when PCNT count reaches Watch Point (10 or 20)
   * Does not directly control GPIO, instead starts hardware timer for 2000us delay
   * 
   * @param unit PCNT unit handle
   * @param edata Watch Point event data (contains trigger value)
   * @param user_ctx User context pointer (this pointer)
   * @return bool Whether to wake higher priority task
   */
  static bool IRAM_ATTR pcnt_on_reach_callback(pcnt_unit_handle_t unit,
                                                const pcnt_watch_event_data_t *edata,
                                                void *user_ctx);

  /**
   * @brief GPTimer alarm interrupt callback function (ISR context)
   * 
   * Executes 2000us after PCNT interrupt is triggered
   * Controls GPIO4 level according to pending_gpio_level_ value
   * 
   * @param timer GPTimer handle
   * @param edata Alarm event data
   * @param user_ctx User context pointer (this pointer)
   * @return bool Whether to wake higher priority task
   */
  static bool IRAM_ATTR timer_alarm_callback(gptimer_handle_t timer,
                                              const gptimer_alarm_event_data_t *edata,
                                              void *user_ctx);
};

}  // namespace zero_cross_relay
}  // namespace esphome
