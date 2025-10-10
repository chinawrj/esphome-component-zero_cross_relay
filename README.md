# Zero-Cross Detection Solid State Relay

## 📋 Component Overview

An ESPHome component for Zero-Cross Detection Solid State Relay (SSR) control, utilizing ESP-IDF native GPIO hardware interrupts for high-precision AC power zero-crossing point detection.

### Core Features

✅ **Hardware Interrupt Driven** - Uses ESP-IDF native GPIO interrupt API with microsecond-level response time  
✅ **Zero-Cross Detection** - Real-time monitoring of AC power zero-crossing points (50Hz/60Hz adaptive)  
✅ **Pulse Width Measurement** - Measures rising edge duration (rising to falling edge time)  
✅ **Frequency Statistics** - Automatic AC power frequency calculation (supports 40Hz-70Hz range)  
✅ **Interrupt Counting** - Records zero-crossing trigger counts for diagnostics  
✅ **IRAM Optimization** - ISR functions execute in IRAM for real-time performance  

### Technical Specifications

| Parameter | Specification |
|-----------|---------------|
| Framework Support | ESP-IDF 5.x (ESP-IDF only) |
| Chip Support | ESP32, ESP32-C6, ESP32-S2, ESP32-S3, etc. |
| Input Signal | AC zero-cross detection (active HIGH) |
| Output Signal | Solid state relay control (active HIGH) |
| Interrupt Type | Both-edge trigger (GPIO_INTR_ANYEDGE) |
| Frequency Range | 40Hz - 70Hz (auto-detect) |
| Response Time | < 10μs (hardware interrupt) |

---

## 🔧 Hardware Connection

### Pin Configuration

| Pin | Direction | Function | Default GPIO | Description |
|-----|-----------|----------|--------------|-------------|
| zero_cross_pin | INPUT | Zero-cross detection input | GPIO3 | Connect to zero-cross detection circuit output (active HIGH) |
| relay_output_pin | OUTPUT | Relay control output | GPIO4 | Connect to solid state relay control terminal (active HIGH) |

### Typical Circuit Connection

```
AC Power ──→ [Zero-Cross Detection Circuit] ──→ GPIO3 (Input)
                                                      ↓
                                              [ESP32 - ISR Handler]
                                                      ↓
                                                GPIO4 (Output) ──→ [Solid State Relay] ──→ Load
```

**Important Notes:**
- Zero-cross detection circuit MUST provide electrical isolation (optocoupler/transformer)
- GPIO3 requires 5V tolerance or level shifter
- Solid state relay should be zero-crossing trigger type

---

## 📦 Installation and Configuration

### 1. ESPHome YAML Configuration

```yaml
# Must use ESP-IDF framework
esp32:
  board: esp32-c6-devkitm-1
  variant: esp32c6
  framework:
    type: esp-idf  # ESP-IDF required
    version: 5.4.2

# Import component from GitHub
external_components:
  - source: github://chinawrj/esphome-component-zero_cross_relay
    components: [ zero_cross_relay ]

# Configure zero-cross relay
zero_cross_relay:
  id: my_zcr
  zero_cross_pin: GPIO3    # Optional, default GPIO3
  relay_output_pin: GPIO4  # Optional, default GPIO4
```

### 2. Custom Pin Configuration

```yaml
zero_cross_relay:
  id: my_zcr
  zero_cross_pin: GPIO5     # Custom zero-cross detection input
  relay_output_pin: GPIO6   # Custom relay output
```

### 3. Compile and Upload

```bash
# Activate ESPHome environment
source ~/venv/esphome/bin/activate

# Compile firmware
esphome compile your_config.yaml

# Upload to device
esphome upload your_config.yaml

# Monitor logs
esphome logs your_config.yaml
```

---

## 📊 Log Output Examples

### Startup Logs

```
[14:23:45][I][zero_cross_relay:22] 🔧 Setting up Zero-Cross Detection Solid State Relay (ESP-IDF Interrupt Mode)...
[14:23:45][I][zero_cross_relay:60] ✓ GPIO3 configured as INPUT with PULLUP (zero-cross detection)
[14:23:45][I][zero_cross_relay:77] ✓ GPIO4 configured as OUTPUT, initialized to LOW (relay off)
[14:23:45][I][zero_cross_relay:100] ✅ Hardware interrupt attached to GPIO3 (ANYEDGE - Both Rising & Falling)
[14:23:45][I][zero_cross_relay:101] ✅ Zero-Cross Relay component initialized successfully (ESP-IDF Interrupt Mode)
```

### Runtime Logs (Output every second)

```
[14:23:46][I][zero_cross_relay:126] 📊 Zero-cross pulse statistics:
[14:23:46][I][zero_cross_relay:127]    ├─ Total interrupts: 200
[14:23:46][I][zero_cross_relay:128]    ├─ Complete pulses: 100
[14:23:46][I][zero_cross_relay:129]    ├─ Pulse width: 50 μs
[14:23:46][I][zero_cross_relay:130]    ├─ Pulse interval: 10000 μs (100.0 Hz)
[14:23:46][I][zero_cross_relay:131]    └─ AC Frequency: 50.00 Hz
```

---

## 🛠️ Technical Implementation Details

### Interrupt Service Routine (ISR)

```cpp
void IRAM_ATTR ZeroCrossRelayComponent::gpio_isr_handler(void *arg) {
    ZeroCrossRelayComponent *component = static_cast<ZeroCrossRelayComponent *>(arg);
    
    // 1. Read timestamp (microsecond precision)
    uint32_t current_time = esp_timer_get_time();
    
    // 2. Read GPIO level to determine edge type
    int gpio_level = gpio_get_level(component->zero_cross_gpio_num_);
    
    // 3. Increment trigger counter
    component->trigger_count_++;
    
    if (gpio_level == 1) {
        // Rising edge (0→1) - Pulse start
        gpio_set_level(component->relay_output_gpio_num_, 1);
        
        // Calculate pulse interval (between consecutive rising edges)
        if (component->last_rising_time_ > 0) {
            uint32_t interval = current_time - component->last_rising_time_;
            if (interval > 7000 && interval < 13000) {
                component->pulse_interval_us_ = interval;
            }
        }
        component->last_rising_time_ = current_time;
    } else {
        // Falling edge (1→0) - Pulse end
        // Calculate pulse width (rising to falling edge duration)
        if (component->last_rising_time_ > 0) {
            uint32_t pulse_width = current_time - component->last_rising_time_;
            if (pulse_width > 0 && pulse_width < 10000) {
                component->pulse_width_us_ = pulse_width;
                component->pulse_count_++;
            }
        }
    }
}
```

### Key Design Points

1. **IRAM_ATTR Marking** - ISR functions must execute in IRAM to avoid Flash access latency
2. **Volatile Variables** - Trigger counters use volatile for thread safety
3. **Dual-Edge Detection** - Both rising and falling edges trigger interrupts
4. **Pulse Width Measurement** - Measures rising edge duration (core feature)
5. **Frequency Calculation** - Based on pulse interval (time between rising edges)

---

## ⚡ Performance Characteristics

### Interrupt Response Time

| Stage | Time | Description |
|-------|------|-------------|
| Hardware interrupt latency | < 1μs | GPIO hardware trigger |
| ISR entry time | < 5μs | FreeRTOS scheduling |
| GPIO output time | < 2μs | Direct register operation |
| **Total response time** | **< 10μs** | Detection to output |

### Frequency Measurement Accuracy

- **Measurement Method**: Consecutive zero-crossing point interval
- **Theoretical Accuracy**: ±0.01 Hz
- **Sampling Rate**: 2 times per AC cycle
- **Filter Range**: 40Hz - 70Hz

### Pulse Width Measurement

- **Measurement Target**: Rising edge duration (rising to falling edge time)
- **Typical Range**: 10 - 500μs (depends on zero-cross detection circuit)
- **Resolution**: 1μs (esp_timer_get_time precision)
- **Update Rate**: Every complete pulse (100Hz for 50Hz AC)

---

## 🔍 Troubleshooting

### Issue 1: Compilation Error "gpio_config not found"

**Cause**: Not using ESP-IDF framework

**Solution**:
```yaml
esp32:
  framework:
    type: esp-idf  # Must use ESP-IDF, not Arduino
```

### Issue 2: ISR Not Triggering

**Checklist**:
1. ✅ Is zero-cross detection circuit output connected to GPIO3?
2. ✅ Is zero-cross signal active HIGH (0V→3.3V)?
3. ✅ Use multimeter to measure GPIO3 voltage changes
4. ✅ Check logs for successful interrupt installation

### Issue 3: Frequency Displays 0.00 Hz

**Cause**: Zero-cross signal frequency out of valid range

**Solution**:
- Check if AC power is normal (should be 50Hz or 60Hz)
- Verify zero-cross detection circuit is working
- Check if `trigger_count` is increasing

### Issue 4: No Output on GPIO4

**Debug Method**:
```cpp
// Add debug code in ISR (temporary)
ESP_EARLY_LOGI("ISR", "Triggered!");
gpio_set_level(component->relay_output_gpio_num_, 1);
```

### Issue 5: Pulse Width Shows 0 μs

**Possible Causes**:
- Zero-cross pulse too narrow (< 1μs)
- Falling edge not detected properly
- Check if `pulse_count_` is incrementing

---

## 📚 API Reference

### Configuration Options

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | ID | Required | Component ID |
| `zero_cross_pin` | GPIO | GPIO3 | Zero-cross detection input pin |
| `relay_output_pin` | GPIO | GPIO4 | Relay control output pin |

### Internal Variables

| Variable | Type | Description |
|----------|------|-------------|
| `trigger_count_` | volatile uint32_t | Total interrupt trigger count |
| `pulse_count_` | volatile uint32_t | Complete pulse count (rising→falling) |
| `last_rising_time_` | volatile uint32_t | Last rising edge timestamp (μs) |
| `pulse_width_us_` | volatile uint32_t | Latest pulse width (μs) |
| `pulse_interval_us_` | volatile uint32_t | Pulse interval (μs) - time between rising edges |
| `estimated_frequency_` | float | Estimated AC frequency (Hz) |

---

## 📝 Development Log

### Version History

- **v1.0.0** (2025-10-10)
  - ✅ Initial release
  - ✅ ESP-IDF hardware interrupt support
  - ✅ Dual-edge (rising + falling) detection
  - ✅ Pulse width measurement feature
  - ✅ Zero-cross detection and frequency statistics
  - ✅ Complete YAML configuration interface

### Known Limitations

1. **ESP-IDF Framework Only** - Arduino framework not compatible
2. **Single Channel Output** - Each component instance supports one relay
3. **No Pulse Width Control** - GPIO4 stays HIGH (can be added in ISR)
4. **No Sensor Integration** - ESPHome sensor interface not yet implemented

### Future Plans

- [ ] Add ESPHome sensor support (frequency sensor)
- [ ] Configurable pulse width (for narrow pulse triggering)
- [ ] Multi-channel support (control multiple relays simultaneously)
- [ ] Phase angle control (dimming functionality)
- [ ] Advanced filtering for noisy AC signals
- [ ] Power consumption estimation based on pulse data

---

## 📄 License

This project follows the ESPHome open-source license.

## 👨‍💻 Author

**chinawrj@gmail.com** - October 10, 2025

Developed with assistance from GitHub Copilot.

## 🙏 Acknowledgments

Special thanks to:
- ESPHome project and community
- ESP-IDF framework developers
- All contributors and users

## � Links

- **GitHub Repository**: https://github.com/chinawrj/esphome-component-zero_cross_relay
- **ESPHome Documentation**: https://esphome.io
- **ESP-IDF Documentation**: https://docs.espressif.com/projects/esp-idf/
