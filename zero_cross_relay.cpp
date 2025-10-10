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
  ESP_LOGI(TAG, "🔧 Setting up Zero-Cross Detection Solid State Relay (ESP-IDF Interrupt Mode with Hardware Timestamp)...");

  // Validate pin configuration
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

  // Get GPIO numbers (convert to ESP-IDF format)
  this->zero_cross_gpio_num_ = static_cast<gpio_num_t>(this->zero_cross_pin_->get_pin());
  this->relay_output_gpio_num_ = static_cast<gpio_num_t>(this->relay_output_pin_->get_pin());

  // ========================================
  // Step 1: Initialize Hardware Timer (GPTimer) with Capture Support
  // ========================================
  ESP_LOGI(TAG, "Initializing GPTimer for hardware timestamp capture via ETM...");
  
  gptimer_config_t timer_config = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = 1000000,  // 1MHz = 1μs resolution
      .flags = {
          .intr_shared = false,
      },
  };
  
  esp_err_t err = gptimer_new_timer(&timer_config, &this->gptimer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to create GPTimer: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // Enable the timer
  err = gptimer_enable(this->gptimer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to enable GPTimer: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // Start the timer (free-running mode)
  err = gptimer_start(this->gptimer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to start GPTimer: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "✓ GPTimer initialized and started (1MHz, free-running mode)");

  // ========================================
  // Step 1.5: Setup ETM (Event Task Matrix) for Hardware Capture
  // This creates a hardware connection: GPIO edge -> Timer capture
  // ========================================
  ESP_LOGI(TAG, "Setting up ETM for hardware timestamp capture...");

  // Create GPIO edge event (triggers on ANY edge)
  gpio_etm_event_config_t gpio_event_config = {
      .edge = GPIO_ETM_EVENT_EDGE_ANY,
  };
  
  err = gpio_new_etm_event(&gpio_event_config, &this->gpio_event_, this->zero_cross_gpio_num_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to create GPIO ETM event: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // Get timer capture task (ETM task that captures timer value)
  gptimer_etm_task_config_t timer_task_config = {
      .task_type = GPTIMER_ETM_TASK_CAPTURE,
  };
  
  err = gptimer_new_etm_task(this->gptimer_, &timer_task_config, &this->timer_task_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to create GPTimer ETM task: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // Allocate ETM channel
  esp_etm_channel_config_t etm_config = {};
  err = esp_etm_new_channel(&etm_config, &this->etm_channel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to create ETM channel: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // Connect GPIO event to Timer capture task via ETM
  err = esp_etm_channel_connect(this->etm_channel_, this->gpio_event_, this->timer_task_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to connect ETM channel: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // Enable the ETM channel
  err = esp_etm_channel_enable(this->etm_channel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to enable ETM channel: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "✓ ETM channel configured: GPIO%d edge -> GPTimer capture (HARDWARE)", 
           this->zero_cross_gpio_num_);

  // ========================================
  // Step 2 & 3: Use ESP-IDF native GPIO interrupt API
  // ========================================
  
  // ========================================
  // Step 2: Configure GPIO3 as INPUT (Zero-Cross Detection)
  // ========================================
  gpio_config_t zero_cross_config = {};
  zero_cross_config.pin_bit_mask = (1ULL << this->zero_cross_gpio_num_);
  zero_cross_config.mode = GPIO_MODE_INPUT;
  zero_cross_config.pull_up_en = GPIO_PULLUP_ENABLE;
  zero_cross_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  zero_cross_config.intr_type = GPIO_INTR_ANYEDGE;  // Both-edge trigger interrupt (rising + falling)
  
  err = gpio_config(&zero_cross_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to configure GPIO%d: %s", this->zero_cross_gpio_num_, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "✓ GPIO%d configured as INPUT with PULLUP (zero-cross detection)", this->zero_cross_gpio_num_);

  // ========================================
  // Step 3: Configure GPIO4 as OUTPUT (Relay Control)
  // ========================================
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

  // ========================================
  // Step 4: Install GPIO ISR Service and Attach Handler
  // ========================================
  err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);  // IRAM flag for faster ISR
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
  ESP_LOGI(TAG, "✅ Zero-Cross Relay component initialized successfully (ESP-IDF Interrupt Mode with Hardware Timestamp)");
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
      ESP_LOGI(TAG, "   ├─ AC Frequency: %.2f Hz", this->estimated_frequency_);
      ESP_LOGI(TAG, "   └─ ⏱️  ISR Latency: %u ns (%.2f μs)", 
               this->isr_latency_ns_, 
               this->isr_latency_ns_ / 1000.0f);
    }
  }
}

void ZeroCrossRelayComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Zero Cross Detection Relay:");
  ESP_LOGCONFIG(TAG, "  Zero-cross detection pin: GPIO%d", this->zero_cross_gpio_num_);
  ESP_LOGCONFIG(TAG, "  Relay output pin: GPIO%d", this->relay_output_gpio_num_);
  ESP_LOGCONFIG(TAG, "  Interrupt trigger mode: ANYEDGE (rising + falling)");
  ESP_LOGCONFIG(TAG, "  Hardware timer: GPTimer @ 1MHz (1μs resolution)");
  ESP_LOGCONFIG(TAG, "  Hardware capture: ETM (Event Task Matrix)");
  ESP_LOGCONFIG(TAG, "    ├─ GPIO edge event -> GPTimer capture task");
  ESP_LOGCONFIG(TAG, "    └─ Zero software overhead for timestamp capture");
  ESP_LOGCONFIG(TAG, "  ISR allocation: ESP_INTR_FLAG_IRAM (optimized)");
}

// ========================================
// ESP-IDF GPIO Interrupt Service Routine (ISR) with Hardware Timestamp
// Uses GPTimer to capture exact interrupt trigger time in hardware
// Must use IRAM_ATTR to ensure execution in IRAM
// ========================================
void IRAM_ATTR ZeroCrossRelayComponent::gpio_isr_handler(void *arg) {
  ZeroCrossRelayComponent *component = static_cast<ZeroCrossRelayComponent *>(arg);
  
  // ========================================
  // CRITICAL: Get HARDWARE-CAPTURED timestamp from ETM
  // The GPIO edge already triggered timer capture via ETM hardware
  // We just need to read the captured value
  // ========================================
  uint64_t hardware_count;
  gptimer_get_captured_count(component->gptimer_, &hardware_count);
  
  // Then capture software timestamp (when ISR actually started executing)
  // IMPORTANT: Must use the SAME timer (GPTimer) for both timestamps!
  // Using esp_timer would compare different time bases (ERROR!)
  uint64_t software_count;
  gptimer_get_raw_count(component->gptimer_, &software_count);
  
  // Calculate ISR latency (difference between hardware trigger and software execution)
  // Both timestamps are from the same GPTimer (1MHz = 1μs resolution)
  // Hardware: captured by ETM at exact GPIO edge moment
  // Software: read when ISR code started executing
  if (software_count >= hardware_count) {
    component->isr_latency_ns_ = (uint32_t)((software_count - hardware_count) * 1000);
  }
  
  // Store timestamps for debugging/logging (both in microseconds)
  component->hardware_timestamp_ = hardware_count;
  component->software_timestamp_ = software_count;
  
  // Get system timestamp for edge timing calculations (use esp_timer for this)
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
