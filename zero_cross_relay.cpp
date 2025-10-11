/**
 * @file zero_cross_relay.cpp
 * @brief Zero-Cross Detection Solid State Relay Component Implementation (ESP-IDF PCNT + CPU Interrupt Version)
 * 
 * Implementation Details:
 * - PCNT Unit: Counts GPIO3 rising edges from 0 to 20 (auto-clear at 20)
 * - Watch Point 1: Count = 10 → Pull GPIO4 LOW (turn off relay)
 * - Watch Point 2: Count = 20 → Pull GPIO4 HIGH (turn on relay) + Clear count
 * - Interrupt Callback: PCNT on_reach event triggers ISR for GPIO control
 * 
 * @author chinawrj@gmail.com
 * @date 2025-10-11
 */

#include "zero_cross_relay.h"
#include "esphome/core/log.h"

// ESP-IDF系统头文件
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace zero_cross_relay {

static const char *const TAG = "zero_cross_relay";

// PCNT Configuration Constants
// Note: ESP-IDF PCNT requires symmetric limit range or low_limit < 0
// We use -20 to +20 range, but only count up from 0, watch at 10 and 20
#define PCNT_LOW_LIMIT      -20   // Must be negative for ESP-IDF PCNT
#define PCNT_HIGH_LIMIT     20    // Positive limit
#define PCNT_WATCH_POINT_HALF   10
#define PCNT_GLITCH_FILTER_NS   1000  // 1us glitch filter (adjust based on signal quality)

// GPTimer Configuration Constants
#define TIMER_DELAY_US      2000  // 2000us (2ms) delay after PCNT interrupt
#define TIMER_RESOLUTION_HZ 1000000  // 1MHz timer resolution (1us per tick)

void ZeroCrossRelayComponent::setup() {
  ESP_LOGI(TAG, "🔧 Setting up Zero-Cross Detection Solid State Relay (ESP-IDF PCNT + CPU Interrupt Mode)...");

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
  // Step 1: Configure GPIO4 as OUTPUT (Relay Control) - Initialize FIRST
  // ========================================
  ESP_LOGI(TAG, "Step 1: Configuring GPIO%d as OUTPUT (relay control)...", this->relay_output_gpio_num_);
  
  gpio_config_t relay_config = {};
  relay_config.pin_bit_mask = (1ULL << this->relay_output_gpio_num_);
  relay_config.mode = GPIO_MODE_OUTPUT;
  relay_config.pull_up_en = GPIO_PULLUP_DISABLE;
  relay_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  relay_config.intr_type = GPIO_INTR_DISABLE;
  
  esp_err_t err = gpio_config(&relay_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to configure GPIO%d: %s", this->relay_output_gpio_num_, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  // Initialize to HIGH (relay on at start, will be controlled by PCNT)
  gpio_set_level(this->relay_output_gpio_num_, 1);
  ESP_LOGI(TAG, "✓ GPIO%d configured as OUTPUT, initialized to HIGH (initial state)", this->relay_output_gpio_num_);

  // ========================================
  // Step 2: Configure GPIO3 as INPUT (for PCNT edge counting)
  // ========================================
  ESP_LOGI(TAG, "Step 2: Configuring GPIO%d as INPUT (zero-cross detection for PCNT)...", this->zero_cross_gpio_num_);
  
  gpio_config_t input_config = {};
  input_config.pin_bit_mask = (1ULL << this->zero_cross_gpio_num_);
  input_config.mode = GPIO_MODE_INPUT;
  input_config.pull_up_en = GPIO_PULLUP_ENABLE;  // Enable pull-up (adjust based on your circuit)
  input_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  input_config.intr_type = GPIO_INTR_DISABLE;    // PCNT handles edge detection, not GPIO interrupt
  
  err = gpio_config(&input_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to configure GPIO%d: %s", this->zero_cross_gpio_num_, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "✓ GPIO%d configured as INPUT with PULLUP", this->zero_cross_gpio_num_);

  // ========================================
  // Step 3: Create and Configure PCNT Unit
  // ========================================
  ESP_LOGI(TAG, "Step 3: Creating PCNT unit (count range: 0-%d)...", PCNT_HIGH_LIMIT);
  
  pcnt_unit_config_t unit_config = {
      .low_limit = PCNT_LOW_LIMIT,
      .high_limit = PCNT_HIGH_LIMIT,
      .flags = {},
  };
  
  err = pcnt_new_unit(&unit_config, &this->pcnt_unit_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to create PCNT unit: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "✓ PCNT unit created (low=%d, high=%d)", PCNT_LOW_LIMIT, PCNT_HIGH_LIMIT);

  // ========================================
  // Step 4: Configure Glitch Filter (optional but recommended)
  // ========================================
  ESP_LOGI(TAG, "Step 4: Configuring glitch filter (%d ns)...", PCNT_GLITCH_FILTER_NS);
  
  pcnt_glitch_filter_config_t filter_config = {
      .max_glitch_ns = PCNT_GLITCH_FILTER_NS,
  };
  
  err = pcnt_unit_set_glitch_filter(this->pcnt_unit_, &filter_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to set glitch filter: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "✓ Glitch filter configured (%d ns)", PCNT_GLITCH_FILTER_NS);

  // ========================================
  // Step 5: Create PCNT Channel and Set Edge Action
  // ========================================
  ESP_LOGI(TAG, "Step 5: Creating PCNT channel for GPIO%d...", this->zero_cross_gpio_num_);
  
  pcnt_chan_config_t channel_config = {
      .edge_gpio_num = this->zero_cross_gpio_num_,
      .level_gpio_num = -1,  // No level control GPIO
      .flags = {},
  };
  
  err = pcnt_new_channel(this->pcnt_unit_, &channel_config, &this->pcnt_channel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to create PCNT channel: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  // Set edge action: Rising edge INCREASE, Falling edge HOLD
  err = pcnt_channel_set_edge_action(this->pcnt_channel_,
                                     PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_HOLD);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to set edge action: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "✓ PCNT channel created (GPIO%d: rising↑ +1, falling↓ hold)", this->zero_cross_gpio_num_);

  // ========================================
  // Step 6: Add Watch Points (configurable flip point and 20)
  // ========================================
  int flip_point = this->duty_cycle_flip_point_;  // Read current duty cycle setting
  ESP_LOGI(TAG, "Step 6: Adding watch points (%d and %d)...", flip_point, PCNT_HIGH_LIMIT);
  
  err = pcnt_unit_add_watch_point(this->pcnt_unit_, flip_point);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to add watch point %d: %s", flip_point, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  err = pcnt_unit_add_watch_point(this->pcnt_unit_, PCNT_HIGH_LIMIT);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to add watch point %d: %s", PCNT_HIGH_LIMIT, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "✓ Watch points added: %d (GPIO4→LOW, duty=%.1f%%), %d (GPIO4→HIGH+clear)", 
           flip_point, (flip_point / 20.0f) * 100.0f, PCNT_HIGH_LIMIT);

  // ========================================
  // Step 7: Register Event Callback
  // ========================================
  ESP_LOGI(TAG, "Step 7: Registering PCNT event callback...");
  
  pcnt_event_callbacks_t callbacks = {
      .on_reach = pcnt_on_reach_callback,
  };
  
  err = pcnt_unit_register_event_callbacks(this->pcnt_unit_, &callbacks, (void *)this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to register event callbacks: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "✓ Event callback registered (on_reach ISR)");

  // ========================================
  // Step 8: Enable and Start PCNT Unit
  // ========================================
  ESP_LOGI(TAG, "Step 8: Enabling and starting PCNT unit...");
  
  err = pcnt_unit_enable(this->pcnt_unit_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to enable PCNT unit: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  err = pcnt_unit_clear_count(this->pcnt_unit_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to clear PCNT count: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  err = pcnt_unit_start(this->pcnt_unit_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to start PCNT unit: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  ESP_LOGI(TAG, "✓ PCNT unit enabled and started (counting from 0)");

  // ========================================
  // Step 9: Create and Configure GPTimer for Delayed GPIO Control
  // ========================================
  ESP_LOGI(TAG, "Step 9: Creating GPTimer for %dus delay...", TIMER_DELAY_US);
  
  gptimer_config_t timer_config = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = TIMER_RESOLUTION_HZ,  // 1MHz = 1us per tick
      .flags = {
          .intr_shared = false,
      },
  };
  
  err = gptimer_new_timer(&timer_config, &this->delay_timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to create GPTimer: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  // Configure timer alarm (one-shot mode, will be restarted in PCNT ISR)
  gptimer_alarm_config_t alarm_config = {
      .alarm_count = TIMER_DELAY_US,  // Alarm at 2000us
      .reload_count = 0,              // Reload to 0
      .flags = {
          .auto_reload_on_alarm = false,  // One-shot mode (manual restart)
      },
  };
  
  err = gptimer_set_alarm_action(this->delay_timer_, &alarm_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to set timer alarm: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  // Register timer alarm callback
  gptimer_event_callbacks_t timer_callbacks = {
      .on_alarm = timer_alarm_callback,
  };
  
  err = gptimer_register_event_callbacks(this->delay_timer_, &timer_callbacks, (void *)this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to register timer callbacks: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  // Enable timer (but don't start yet - will be started by PCNT ISR)
  err = gptimer_enable(this->delay_timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to enable GPTimer: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  ESP_LOGI(TAG, "✓ GPTimer configured (one-shot, %dus delay)", TIMER_DELAY_US);
  
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "✅ Zero-Cross Relay initialized successfully!");
  ESP_LOGI(TAG, "   ├─ Input: GPIO%d (rising edge counts)", this->zero_cross_gpio_num_);
  ESP_LOGI(TAG, "   ├─ Output: GPIO%d (controlled via delayed timer)", this->relay_output_gpio_num_);
  ESP_LOGI(TAG, "   ├─ Count range: %d-%d (auto-clear at %d)", 
           PCNT_LOW_LIMIT, PCNT_HIGH_LIMIT, PCNT_HIGH_LIMIT);
  ESP_LOGI(TAG, "   ├─ Duty cycle: %.1f%% (flip point=%d, range: 1-19)", 
           (this->duty_cycle_flip_point_ / 20.0f) * 100.0f, this->duty_cycle_flip_point_);
  ESP_LOGI(TAG, "   ├─ Watch point 1: Count=%d → Start timer → %dus → GPIO4 LOW", 
           this->duty_cycle_flip_point_, TIMER_DELAY_US);
  ESP_LOGI(TAG, "   └─ Watch point 2: Count=%d → Start timer → %dus → GPIO4 HIGH + clear", 
           PCNT_HIGH_LIMIT, TIMER_DELAY_US);
}

void ZeroCrossRelayComponent::loop() {
  // ========================================
  // Periodic status logging (every 5 seconds)
  // ========================================
  
  static uint32_t last_log_time = 0;
  uint32_t current_time = millis();
  
  // Output statistics every 5 seconds
  if (current_time - last_log_time > 5000) {
    last_log_time = current_time;
    
    // Read current PCNT count
    int pcnt_count = 0;
    esp_err_t err = pcnt_unit_get_count(this->pcnt_unit_, &pcnt_count);
    
    if (err == ESP_OK) {
      // Get cycle statistics from ISR (atomic read)
      uint32_t total_triggers = this->trigger_count_;
      uint32_t total_cycles = this->cycle_count_;
      
      // Calculate cycle time if we have at least one complete cycle
      float cycle_time_ms = 0.0f;
      if (total_cycles > 1 && this->last_cycle_time_ > 0) {
        // Get cycle time in milliseconds (us → ms)
        cycle_time_ms = (float)this->last_cycle_time_ / 1000.0f;
        
        // Calculate estimated AC frequency
        // Logic:
        // - 20 zero-cross pulses per cycle (PCNT counts 0→20)
        // - For 50Hz AC: 100 zero-cross points/second
        // - So 20 pulses = 20/100 = 0.2 seconds = 200ms
        // - Frequency = (20 pulses) / (cycle_time_seconds) / 2
        // - Formula: freq = 20 / (cycle_time_ms / 1000) / 2 = 10000 / cycle_time_ms
        if (cycle_time_ms > 0) {
          this->estimated_frequency_ = 10000.0f / cycle_time_ms;
        }
      }
      
      ESP_LOGI(TAG, "📊 PCNT Zero-Cross Statistics:");
      ESP_LOGI(TAG, "   ├─ Current count: %d / %d", pcnt_count, PCNT_HIGH_LIMIT);
      ESP_LOGI(TAG, "   ├─ Duty cycle: %.1f%% (flip point: %d)", 
               (this->duty_cycle_flip_point_ / 20.0f) * 100.0f, this->duty_cycle_flip_point_);
      ESP_LOGI(TAG, "   ├─ Total watch point triggers: %u", total_triggers);
      ESP_LOGI(TAG, "   ├─ Complete cycles (20-count): %u", total_cycles);
      if (cycle_time_ms > 0) {
        ESP_LOGI(TAG, "   ├─ Last cycle time: %.2f ms", cycle_time_ms);
        ESP_LOGI(TAG, "   └─ Estimated AC frequency: %.2f Hz", this->estimated_frequency_);
      } else {
        ESP_LOGI(TAG, "   └─ (Waiting for first complete cycle...)");
      }
    }
  }
}

void ZeroCrossRelayComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Zero Cross Detection Relay (PCNT + GPTimer Mode):");
  ESP_LOGCONFIG(TAG, "  Zero-cross input: GPIO%d (PCNT edge counting)", this->zero_cross_gpio_num_);
  ESP_LOGCONFIG(TAG, "  Relay output: GPIO%d (controlled by GPTimer delayed)", this->relay_output_gpio_num_);
  ESP_LOGCONFIG(TAG, "  Count range: %d - %d (auto-clear at %d)", 
                PCNT_LOW_LIMIT, PCNT_HIGH_LIMIT, PCNT_HIGH_LIMIT);
  ESP_LOGCONFIG(TAG, "  Duty cycle control:");
  ESP_LOGCONFIG(TAG, "    ├─ Current duty cycle: %.1f%% (flip point: %d)", 
                (this->duty_cycle_flip_point_ / 20.0f) * 100.0f, this->duty_cycle_flip_point_);
  ESP_LOGCONFIG(TAG, "    └─ Adjustable range: 5%% - 95%% (flip point: 1-19)");
  ESP_LOGCONFIG(TAG, "  Watch points (with %dus delay):", TIMER_DELAY_US);
  ESP_LOGCONFIG(TAG, "    ├─ Point 1: Count=%d → GPIO%d LOW (relay off)", 
                this->duty_cycle_flip_point_, this->relay_output_gpio_num_);
  ESP_LOGCONFIG(TAG, "    └─ Point 2: Count=%d → GPIO%d HIGH (relay on) + clear count", 
                PCNT_HIGH_LIMIT, this->relay_output_gpio_num_);
  ESP_LOGCONFIG(TAG, "  Edge action: Rising edge +1, Falling edge HOLD");
  ESP_LOGCONFIG(TAG, "  Glitch filter: %d ns", PCNT_GLITCH_FILTER_NS);
}

// ========================================
// PCNT Watch Point Interrupt Callback (ISR Context)
// Triggered when PCNT count reaches watch points (10 or 20)
// Does NOT directly control GPIO - instead starts hardware timer for delayed control
// Must use IRAM_ATTR to ensure execution in IRAM
// ========================================
bool IRAM_ATTR ZeroCrossRelayComponent::pcnt_on_reach_callback(pcnt_unit_handle_t unit,
                                                               const pcnt_watch_event_data_t *edata,
                                                               void *user_ctx) {
  ZeroCrossRelayComponent *component = static_cast<ZeroCrossRelayComponent *>(user_ctx);
  
  int watch_point_value = edata->watch_point_value;
  
  // Increment total trigger counter
  component->trigger_count_++;
  
  // Check if this is the duty cycle flip point (dynamic value, not fixed at 10)
  if (watch_point_value == component->duty_cycle_flip_point_) {
    // ========================================
    // Watch Point 1: Count = duty_cycle_flip_point (1-19, configurable)
    // Set pending GPIO level to LOW, then start timer
    // ========================================
    component->pending_gpio_level_ = 0;  // Prepare to set GPIO LOW
    
    // Start one-shot timer (will fire after 2000us)
    gptimer_set_raw_count(component->delay_timer_, 0);  // Reset timer count to 0
    gptimer_start(component->delay_timer_);             // Start timer
    
  } else if (watch_point_value == PCNT_HIGH_LIMIT) {
    // ========================================
    // Watch Point 2: Count = 20
    // Set pending GPIO level to HIGH, then start timer
    // ========================================
    component->pending_gpio_level_ = 1;  // Prepare to set GPIO HIGH
    
    // Record cycle completion time (for frequency calculation)
    uint32_t current_time = esp_timer_get_time();
    static uint32_t last_timestamp = 0;  // Static variable to store last cycle timestamp
    
    if (last_timestamp > 0) {
      // Calculate time elapsed for this 20-count cycle (in microseconds)
      component->last_cycle_time_ = current_time - last_timestamp;
    }
    
    // Update timestamp for next cycle
    last_timestamp = current_time;
    
    // Increment cycle counter
    component->cycle_count_++;
    
    // Clear PCNT count to restart from 0
    pcnt_unit_clear_count(unit);
    
    // Start one-shot timer (will fire after 2000us)
    gptimer_set_raw_count(component->delay_timer_, 0);  // Reset timer count to 0
    gptimer_start(component->delay_timer_);             // Start timer
  }
  
  // Return false: no need to wake higher priority task
  return false;
}

// ========================================
// GPTimer Alarm Interrupt Callback (ISR Context)
// Triggered 2000us after PCNT interrupt
// Performs the actual GPIO control based on pending_gpio_level_
// Must use IRAM_ATTR to ensure execution in IRAM
// ========================================
bool IRAM_ATTR ZeroCrossRelayComponent::timer_alarm_callback(gptimer_handle_t timer,
                                                             const gptimer_alarm_event_data_t *edata,
                                                             void *user_ctx) {
  ZeroCrossRelayComponent *component = static_cast<ZeroCrossRelayComponent *>(user_ctx);
  
  // Stop timer (one-shot mode)
  gptimer_stop(timer);
  
  // Execute delayed GPIO control
  if (component->pending_gpio_level_ >= 0) {
    gpio_set_level(component->relay_output_gpio_num_, component->pending_gpio_level_);
    component->pending_gpio_level_ = -1;  // Clear pending state
  }
  
  // Return false: no need to wake higher priority task
  return false;
}

}  // namespace zero_cross_relay
}  // namespace esphome
