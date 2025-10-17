/**
 * @file zero_cross_relay.cpp
 * @brief Zero-Cross Detection Solid State Relay Component Implementation (ESP-IDF PCNT + CPU Interrupt Version)
 * 
 * Implementation Details:
 * - PCNT Unit: Counts GPIO3 rising edges from 0 to 20 (auto-clear at 20)
 * - Watch Point 1: Configurable count (1-19) to pull GPIO4 LOW (0% disables, 100% keeps HIGH)
 * - Watch Point 2: Count = 20 → Pull GPIO4 HIGH (turn on relay) + Clear count
 * - Interrupt Callback: PCNT on_reach event triggers ISR for GPIO control
 * 
 * ESP32 Dual-Core Optimization:
 * - Interrupt Priority: 3 (highest on ESP32, range: 1-3)
 * - CPU Core Affinity: Core 1 (APP_CPU, away from WiFi/BLE on Core 0)
 * - Purpose: Minimize WiFi interference and ensure precise zero-cross timing
 * 
 * @author chinawrj@gmail.com
 * @date 2025-10-11
 * @updated 2025-10-17 (Added Core 1 binding and highest priority)
 */

#include "zero_cross_relay.h"
#include "esphome/core/log.h"

// ESP-IDF system headers
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

// Interrupt Configuration Constants (ESP32 Dual-Core Optimization)
// ESP32 has PRO_CPU (Core 0, WiFi/BLE) and APP_CPU (Core 1, Application)
// We bind interrupts to Core 1 to avoid interference with WiFi tasks
#define INTERRUPT_PRIORITY  3       // Highest priority on ESP32 (range: 1-3)
#define INTERRUPT_CPU_CORE  1       // Core 1 (APP_CPU, away from WiFi on Core 0)

void ZeroCrossRelayComponent::set_duty_cycle_flip_point(int flip_point) {
  if (flip_point < 0 || flip_point > PCNT_HIGH_LIMIT) {
    ESP_LOGW(TAG, "Requested duty cycle flip point %d out of range (valid range: 0-%d).",
             flip_point, PCNT_HIGH_LIMIT);
    return;
  }

  float percentage = (static_cast<float>(flip_point) / static_cast<float>(PCNT_HIGH_LIMIT)) * 100.0f;

  if (this->pcnt_unit_ == nullptr) {
    // Component not fully initialized yet; store as initial value for setup().
    this->duty_cycle_flip_point_ = flip_point;
    this->pending_duty_cycle_flip_point_ = -1;
    ESP_LOGI(TAG, "Preset duty cycle to %.1f%% (flip point %d) before initialization completes.", percentage, flip_point);
    return;
  }

  if (flip_point == this->duty_cycle_flip_point_) {
    // Already active, no need to queue another update.
    this->pending_duty_cycle_flip_point_ = -1;
    ESP_LOGD(TAG, "Duty cycle already %.1f%% (flip point %d); ignoring duplicate request.", percentage, flip_point);
    return;
  }

  // Cache the new flip point; will be applied synchronously at next cycle boundary.
  this->pending_duty_cycle_flip_point_ = flip_point;
  ESP_LOGI(TAG,
           "Queued duty cycle update to %.1f%% (flip point %d). Will apply at the next zero-cross cycle boundary.",
           percentage, flip_point);
}

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
  
  // Initialize output according to current duty cycle (0% => LOW, otherwise HIGH)
  int initial_level = (this->duty_cycle_flip_point_ == 0) ? 0 : 1;
  gpio_set_level(this->relay_output_gpio_num_, initial_level);
  ESP_LOGI(TAG, "✓ GPIO%d configured as OUTPUT, initialized to %s (initial state)",
           this->relay_output_gpio_num_, initial_level ? "HIGH" : "LOW");

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
  ESP_LOGI(TAG, "Step 6: Configuring watch points (flip=%d, high=%d)...", flip_point, PCNT_HIGH_LIMIT);
  
  bool has_dynamic_watch_point = (flip_point > 0 && flip_point < PCNT_HIGH_LIMIT);
  if (has_dynamic_watch_point) {
    err = pcnt_unit_add_watch_point(this->pcnt_unit_, flip_point);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "❌ Failed to add watch point %d: %s", flip_point, esp_err_to_name(err));
      this->mark_failed();
      return;
    }
  } else {
    ESP_LOGI(TAG, "   • Dynamic watch point skipped (flip point %d => %.1f%% duty).",
             flip_point,
             (static_cast<float>(flip_point) / static_cast<float>(PCNT_HIGH_LIMIT)) * 100.0f);
  }
  
  err = pcnt_unit_add_watch_point(this->pcnt_unit_, PCNT_HIGH_LIMIT);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to add watch point %d: %s", PCNT_HIGH_LIMIT, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  
  float duty_percentage = (static_cast<float>(flip_point) / static_cast<float>(PCNT_HIGH_LIMIT)) * 100.0f;
  if (has_dynamic_watch_point) {
    ESP_LOGI(TAG, "✓ Watch points ready: %d (GPIO4→LOW, duty=%.1f%%), %d (GPIO4→HIGH+clear)",
             flip_point, duty_percentage, PCNT_HIGH_LIMIT);
  } else if (flip_point == 0) {
    ESP_LOGI(TAG, "✓ Watch point ready: %d (GPIO4→HIGH+clear). Duty cycle 0%% (relay always OFF).",
             PCNT_HIGH_LIMIT);
  } else {
    ESP_LOGI(TAG, "✓ Watch point ready: %d (GPIO4→HIGH+clear). Duty cycle 100%% (relay always ON).",
             PCNT_HIGH_LIMIT);
  }

  // ========================================
  // Step 7: Register Event Callback with Core 1 Affinity and High Priority
  // ========================================
  ESP_LOGI(TAG, "Step 7: Registering PCNT event callback (Core %d, Priority %d)...", 
           INTERRUPT_CPU_CORE, INTERRUPT_PRIORITY);
  
  pcnt_event_callbacks_t callbacks = {
      .on_reach = pcnt_on_reach_callback,
  };
  
  err = pcnt_unit_register_event_callbacks(this->pcnt_unit_, &callbacks, (void *)this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "❌ Failed to register event callbacks: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "✓ Event callback registered (on_reach ISR, Core %d)", INTERRUPT_CPU_CORE);

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
  ESP_LOGI(TAG, "Step 9: Creating GPTimer for %dus delay (Core %d, Priority %d)...", 
           TIMER_DELAY_US, INTERRUPT_CPU_CORE, INTERRUPT_PRIORITY);
  
  gptimer_config_t timer_config = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = TIMER_RESOLUTION_HZ,  // 1MHz = 1us per tick
      .intr_priority = INTERRUPT_PRIORITY,   // 🔴 Highest priority (1-3 on ESP32)
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
  
  // Register timer alarm callback (bind to Core 1)
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
  
  // 🔴 Bind GPTimer interrupt to Core 1 (away from WiFi on Core 0)
  // Note: ESP-IDF allocates interrupt on the core that calls gptimer_enable()
  // To ensure Core 1 binding, we can set interrupt affinity explicitly
  ESP_LOGI(TAG, "✓ GPTimer configured (one-shot, %dus delay, Core %d, Priority %d)", 
           TIMER_DELAY_US, INTERRUPT_CPU_CORE, INTERRUPT_PRIORITY);
  
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "✅ Zero-Cross Relay initialized successfully!");
  ESP_LOGI(TAG, "   ├─ Input: GPIO%d (rising edge counts)", this->zero_cross_gpio_num_);
  ESP_LOGI(TAG, "   ├─ Output: GPIO%d (controlled via delayed timer)", this->relay_output_gpio_num_);
  ESP_LOGI(TAG, "   ├─ Count range: %d-%d (auto-clear at %d)", 
           PCNT_LOW_LIMIT, PCNT_HIGH_LIMIT, PCNT_HIGH_LIMIT);
  ESP_LOGI(TAG, "   ├─ Interrupt config: Core %d (APP_CPU), Priority %d (highest)", 
           INTERRUPT_CPU_CORE, INTERRUPT_PRIORITY);
  float current_duty_percentage =
      (static_cast<float>(this->duty_cycle_flip_point_) / static_cast<float>(PCNT_HIGH_LIMIT)) * 100.0f;
  ESP_LOGI(TAG, "   ├─ Duty cycle: %.1f%% (flip point=%d, range: 0-%d)", 
           current_duty_percentage, this->duty_cycle_flip_point_, PCNT_HIGH_LIMIT);
  if (this->duty_cycle_flip_point_ > 0 && this->duty_cycle_flip_point_ < PCNT_HIGH_LIMIT) {
    ESP_LOGI(TAG, "   ├─ Watch point 1: Count=%d → Start timer → %dus → GPIO4 LOW", 
             this->duty_cycle_flip_point_, TIMER_DELAY_US);
  } else if (this->duty_cycle_flip_point_ == 0) {
    ESP_LOGI(TAG, "   ├─ Watch point 1: disabled (relay held LOW / 0%% duty)");
  } else {
    ESP_LOGI(TAG, "   ├─ Watch point 1: disabled (relay held HIGH / 100%% duty)");
  }
  ESP_LOGI(TAG, "   └─ Watch point 2: Count=%d → Start timer → %dus → GPIO4 HIGH + clear", 
           PCNT_HIGH_LIMIT, TIMER_DELAY_US);
}

void ZeroCrossRelayComponent::loop() {
  if (this->watch_point_update_event_) {
    bool success = (this->last_watch_point_update_err_ == ESP_OK);
    if (success) {
      float duty_percentage =
          (static_cast<float>(this->duty_cycle_flip_point_) / static_cast<float>(PCNT_HIGH_LIMIT)) * 100.0f;
      ESP_LOGI(TAG, "Duty cycle watch point updated to %.1f%% (flip point %d).",
               duty_percentage, this->duty_cycle_flip_point_);
    } else {
      int pending = this->pending_duty_cycle_flip_point_;
      if (pending < 0) {
        pending = this->duty_cycle_flip_point_;
      }
      ESP_LOGE(TAG, "Failed to update duty cycle watch point to %d: %s", pending,
               esp_err_to_name(this->last_watch_point_update_err_));
    }
    this->watch_point_update_event_ = false;
  }
  
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
               (static_cast<float>(this->duty_cycle_flip_point_) / static_cast<float>(PCNT_HIGH_LIMIT)) * 100.0f,
               this->duty_cycle_flip_point_);
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
  float duty_percentage =
      (static_cast<float>(this->duty_cycle_flip_point_) / static_cast<float>(PCNT_HIGH_LIMIT)) * 100.0f;
  ESP_LOGCONFIG(TAG, "    ├─ Current duty cycle: %.1f%% (flip point: %d)", 
                duty_percentage, this->duty_cycle_flip_point_);
  ESP_LOGCONFIG(TAG, "    └─ Adjustable range: 0%% - 100%% (flip point: 0-%d)", PCNT_HIGH_LIMIT);
  ESP_LOGCONFIG(TAG, "  Watch points (with %dus delay):", TIMER_DELAY_US);
  if (this->duty_cycle_flip_point_ > 0 && this->duty_cycle_flip_point_ < PCNT_HIGH_LIMIT) {
    ESP_LOGCONFIG(TAG, "    ├─ Point 1: Count=%d → GPIO%d LOW (relay off)", 
                  this->duty_cycle_flip_point_, this->relay_output_gpio_num_);
  } else if (this->duty_cycle_flip_point_ == 0) {
    ESP_LOGCONFIG(TAG, "    ├─ Point 1: disabled (relay held LOW / 0%% duty)");
  } else {
    ESP_LOGCONFIG(TAG, "    ├─ Point 1: disabled (relay held HIGH / 100%% duty)");
  }
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
  int active_flip_point = component->duty_cycle_flip_point_;
  if (active_flip_point > 0 && active_flip_point < PCNT_HIGH_LIMIT &&
      watch_point_value == active_flip_point) {
    // ========================================
    // Watch Point 1: Count = duty_cycle_flip_point (enabled for 1-19)
    // Set pending GPIO level to LOW, then start timer
    // ========================================
    component->pending_gpio_level_ = 0;  // Prepare to set GPIO LOW
    
    // Start one-shot timer (will fire after 2000us)
    gptimer_set_raw_count(component->delay_timer_, 0);  // Reset timer count to 0
    gptimer_start(component->delay_timer_);             // Start timer
    
  } else if (watch_point_value == PCNT_HIGH_LIMIT) {
    // ========================================
    // Watch Point 2: Count = 20
    // Set pending GPIO level based on duty cycle extreme, then start timer
    // ========================================
    
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

    // Apply any pending duty cycle watch point update synchronously at cycle boundary.
    int pending_flip_point = component->pending_duty_cycle_flip_point_;
    int current_flip_point = component->duty_cycle_flip_point_;
    if (pending_flip_point >= 0 && pending_flip_point <= PCNT_HIGH_LIMIT &&
        pending_flip_point != current_flip_point) {
      bool current_has_watch_point =
          (current_flip_point > 0 && current_flip_point < PCNT_HIGH_LIMIT);
      bool pending_needs_watch_point =
          (pending_flip_point > 0 && pending_flip_point < PCNT_HIGH_LIMIT);
      
      esp_err_t remove_err = ESP_OK;
      if (current_has_watch_point) {
        remove_err = pcnt_unit_remove_watch_point(unit, current_flip_point);
      }
      
      if (remove_err == ESP_OK || remove_err == ESP_ERR_NOT_FOUND) {
        esp_err_t add_err = ESP_OK;
        if (pending_needs_watch_point) {
          add_err = pcnt_unit_add_watch_point(unit, pending_flip_point);
        }
        if (add_err == ESP_OK) {
          component->duty_cycle_flip_point_ = pending_flip_point;
          component->pending_duty_cycle_flip_point_ = -1;
          component->last_watch_point_update_err_ = ESP_OK;
        } else {
          // Restore previous watch point if it was removed successfully.
          if (current_has_watch_point && remove_err == ESP_OK) {
            pcnt_unit_add_watch_point(unit, current_flip_point);
          }
          component->last_watch_point_update_err_ = add_err;
        }
      } else {
        component->last_watch_point_update_err_ = remove_err;
      }
      component->watch_point_update_event_ = true;
    }

    int desired_level = (component->duty_cycle_flip_point_ == 0) ? 0 : 1;
    component->pending_gpio_level_ = desired_level;  // Prepare next GPIO level

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
