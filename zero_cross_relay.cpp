/**
 * @file zero_cross_relay.cpp
 * @brief Zero-Cross Detection Solid State Relay Component Implementation (ESP-IDF Hardware Interrupt Version)
 * 
 * @author chinawrj@gmail.com
 * @date 2025-10-10
 */

#include "zero_cross_relay.h"
#include "esphome/core/log.h"

// ESP-IDF系统头文件
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace zero_cross_relay {

static const char *const TAG = "zero_cross_relay";

void ZeroCrossRelayComponent::setup() {
  ESP_LOGI(TAG, "🔧 Setting up Zero-Cross Detection Solid State Relay (ESP-IDF Interrupt Mode)...");

  // 验证引脚配置
  if (this->zero_cross_pin_ == nullptr) {
    ESP_LOGE(TAG, "❌ Zero-cross detection pin not configured!");
    this->mark_failed();
    return;
  }

  if (this->relay_output_pin_ == nullptr) {
    ESP_LOGE(TAG, "❌ Relay output pin not configured!");
    this->mark_failed();
    return;
  }

  // 获取GPIO编号（转换为ESP-IDF格式）
  this->zero_cross_gpio_num_ = static_cast<gpio_num_t>(this->zero_cross_pin_->get_pin());
  this->relay_output_gpio_num_ = static_cast<gpio_num_t>(this->relay_output_pin_->get_pin());

  // ========================================
  // Step 2 & 3: Use ESP-IDF native GPIO interrupt API
  // ========================================
  
  // 1. Configure GPIO3: Zero-cross detection input (using ESP-IDF API)
  gpio_config_t zero_cross_config = {};
  zero_cross_config.pin_bit_mask = (1ULL << this->zero_cross_gpio_num_);
  zero_cross_config.mode = GPIO_MODE_INPUT;
  zero_cross_config.pull_up_en = GPIO_PULLUP_ENABLE;
  zero_cross_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  zero_cross_config.intr_type = GPIO_INTR_ANYEDGE;  // Both-edge trigger interrupt (rising + falling)
  
  esp_err_t err = gpio_config(&zero_cross_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to configure GPIO%d: %s", this->zero_cross_gpio_num_, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "✓ GPIO%d configured as INPUT with PULLUP (zero-cross detection)", this->zero_cross_gpio_num_);

  // 2. Configure GPIO4: Relay output (using ESP-IDF API)
  gpio_config_t relay_config = {};
  relay_config.pin_bit_mask = (1ULL << this->relay_output_gpio_num_);
  relay_config.mode = GPIO_MODE_OUTPUT;
  relay_config.pull_up_en = GPIO_PULLUP_DISABLE;
  relay_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  relay_config.intr_type = GPIO_INTR_DISABLE;
  
  err = gpio_config(&relay_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to configure GPIO%d: %s", this->relay_output_gpio_num_, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  // Initialize to LOW (relay off)
  gpio_set_level(this->relay_output_gpio_num_, 0);
  ESP_LOGI(TAG, "✓ GPIO%d configured as OUTPUT, initialized to LOW (relay off)", this->relay_output_gpio_num_);

  // 3. Install GPIO interrupt service
  err = gpio_install_isr_service(0);  // 0 = default flags
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // ESP_ERR_INVALID_STATE means already installed
    ESP_LOGE(TAG, "❌ Failed to install GPIO ISR service: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  // 4. Register interrupt handler function
  err = gpio_isr_handler_add(this->zero_cross_gpio_num_, gpio_isr_handler, (void *)this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to add ISR handler for GPIO%d: %s", this->zero_cross_gpio_num_, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  ESP_LOGI(TAG, "✅ Hardware interrupt attached to GPIO%d (ANYEDGE - Both Rising & Falling)", this->zero_cross_gpio_num_);
  ESP_LOGI(TAG, "✅ Zero-Cross Relay component initialized successfully (ESP-IDF Interrupt Mode)");
}

void ZeroCrossRelayComponent::loop() {
  // ========================================
  // Step 5: Frequency calculation and log output (in main loop)
  // ========================================
  
  // Calculate frequency (based on pulse interval measured by ISR)
  static uint32_t last_log_time = 0;
  uint32_t current_time = millis();
  
  // Output statistics once per second
  if (current_time - last_log_time > 1000) {
    last_log_time = current_time;
    
    // Get latest measurement data from ISR (atomic read)
    uint32_t pulse_width = this->pulse_width_us_;
    uint32_t pulse_interval = this->pulse_interval_us_;
    
    if (pulse_interval > 0) {
      // Zero-cross pulse frequency (e.g., 50Hz AC generates 100Hz pulses)
      float pulse_freq = 1000000.0f / pulse_interval;
      // AC frequency = pulse frequency / 2 (because one AC cycle has 2 zero-cross points)
      this->estimated_frequency_ = pulse_freq / 2.0f;
      
      ESP_LOGI(TAG, "📊 Zero-cross pulse statistics:");
      ESP_LOGI(TAG, "   ├─ Total interrupts: %u", this->trigger_count_);
      ESP_LOGI(TAG, "   ├─ Complete pulses: %u", this->pulse_count_);
      ESP_LOGI(TAG, "   ├─ Pulse width: %u μs", pulse_width);
      ESP_LOGI(TAG, "   ├─ Pulse interval: %u μs (%.1f Hz)", pulse_interval, pulse_freq);
      ESP_LOGI(TAG, "   └─ AC Frequency: %.2f Hz", this->estimated_frequency_);
    }
  }
}

void ZeroCrossRelayComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Zero-Cross Detection Solid State Relay (Pulse Width Measurement):");
  LOG_PIN("  Zero-Cross Input Pin: ", this->zero_cross_pin_);
  LOG_PIN("  Relay Output Pin: ", this->relay_output_pin_);
  ESP_LOGCONFIG(TAG, "  Interrupt Mode: ANYEDGE (Both Rising & Falling)");
  ESP_LOGCONFIG(TAG, "  Total Interrupts: %u", this->trigger_count_);
  ESP_LOGCONFIG(TAG, "  Complete Pulses: %u", this->pulse_count_);
  ESP_LOGCONFIG(TAG, "  Pulse Width: %u μs", this->pulse_width_us_);
  ESP_LOGCONFIG(TAG, "  Pulse Interval: %u μs", this->pulse_interval_us_);
  ESP_LOGCONFIG(TAG, "  Estimated AC Frequency: %.2f Hz", this->estimated_frequency_);
}

// ========================================
// ESP-IDF GPIO Interrupt Service Routine (ISR) - Zero-Cross Pulse Width Measurement
// Measurement target: Rising edge duration (pulse width from rising to falling edge)
// Must use IRAM_ATTR to ensure execution in IRAM
// ========================================
void IRAM_ATTR ZeroCrossRelayComponent::gpio_isr_handler(void *arg) {
  ZeroCrossRelayComponent *component = static_cast<ZeroCrossRelayComponent *>(arg);
  
  // Read current timestamp (microsecond precision)
  uint32_t current_time = esp_timer_get_time();
  
  // ========================================
  // Key: Read GPIO level to determine rising or falling edge
  // ========================================
  int gpio_level = gpio_get_level(component->zero_cross_gpio_num_);
  
  // Increment total trigger counter
  component->trigger_count_++;
  
  if (gpio_level == 1) {
    // ========================================
    // Rising edge trigger (0→1) - Zero-cross pulse start
    // ========================================
    
    // 1. Output HIGH to GPIO4 (solid state relay ON)
    gpio_set_level(component->relay_output_gpio_num_, 1);
    
    // 2. Calculate pulse interval (time between two consecutive rising edges)
    //    This interval reflects the zero-cross detection frequency
    //    For 50Hz AC, should be 10ms (100Hz pulse frequency)
    if (component->last_rising_time_ > 0) {
      uint32_t interval = current_time - component->last_rising_time_;
      
      // Valid range check:
      // - 50Hz AC → 100Hz pulse → 10000us interval
      // - 60Hz AC → 120Hz pulse → 8333us interval
      // - Allowed range: 40-70Hz AC → 7143us-12500us pulse interval
      if (interval > 7000 && interval < 13000) {
        component->pulse_interval_us_ = interval;
      }
    }
    
    // 3. Record rising edge timestamp (for subsequent pulse width calculation)
    component->last_rising_time_ = current_time;
    
  } else {
    // ========================================
    // Falling edge trigger (1→0) - Zero-cross pulse end
    // ========================================
    
    // 1. Calculate pulse width (duration from rising to falling edge)
    //    This is the core data you requested to measure
    if (component->last_rising_time_ > 0) {
      uint32_t pulse_width = current_time - component->last_rising_time_;
      
      // Validity check: Zero-cross pulse width is typically from a few to a few hundred microseconds
      // Should not exceed half a cycle (e.g., half cycle of 50Hz is 10ms)
      if (pulse_width > 0 && pulse_width < 10000) {
        component->pulse_width_us_ = pulse_width;
        component->pulse_count_++;  // Complete pulse count
      }
    }
    
    // 2. Optional: Turn off relay on falling edge (synchronized with pulse)
    // gpio_set_level(component->relay_output_gpio_num_, 0);
  }
  
  // ========================================
  // Optional feature: Fixed pulse width output
  // ========================================
  // If fixed-width pulse output to GPIO4 is needed, delay after rising edge then turn off
  // Example: 100us fixed pulse width
  // if (gpio_level == 1) {
  //   ets_delay_us(100);
  //   gpio_set_level(component->relay_output_gpio_num_, 0);
  // }
}

}  // namespace zero_cross_relay
}  // namespace esphome
