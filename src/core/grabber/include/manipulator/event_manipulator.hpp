#pragma once

#include "boost_defs.hpp"

#include "gcd_utility.hpp"
#include "logger.hpp"
#include "manipulator.hpp"
#include "modifier_flag_manager.hpp"
#include "pointing_button_manager.hpp"
#include "system_preferences.hpp"
#include "types.hpp"
#include "virtual_hid_device_client.hpp"
#include <IOKit/hidsystem/ev_keymap.h>
#include <boost/optional.hpp>
#include <list>
#include <thread>
#include <unordered_map>

namespace manipulator {
class event_manipulator final {
public:
  event_manipulator(const event_manipulator&) = delete;

  event_manipulator(virtual_hid_device_client& virtual_hid_device_client) : virtual_hid_device_client_(virtual_hid_device_client),
                                                                            last_timestamp_(0) {
  }

  ~event_manipulator(void) {
  }

  enum class ready_state {
    ready,
    virtual_hid_device_client_is_not_ready,
    virtual_hid_keyboard_is_not_ready,
  };

  ready_state is_ready(void) {
    if (!virtual_hid_device_client_.is_connected()) {
      return ready_state::virtual_hid_device_client_is_not_ready;
    }
    if (!virtual_hid_device_client_.is_virtual_hid_keyboard_initialized()) {
      return ready_state::virtual_hid_keyboard_is_not_ready;
    }
    return ready_state::ready;
  }

  void reset(void) {
    manipulated_keys_.clear();
    manipulated_fn_keys_.clear();

    reset_coder_layout();

    modifier_flag_manager_.reset();
    modifier_flag_manager_.unlock();

    pointing_button_manager_.reset();

    // Do not call terminate_virtual_hid_keyboard
    virtual_hid_device_client_.terminate_virtual_hid_pointing();
  }

  void reset_modifier_flag_state(void) {
    modifier_flag_manager_.reset();
    // Do not call modifier_flag_manager_.unlock() here.
  }

  void reset_pointing_button_state(void) {
    auto bits = pointing_button_manager_.get_hid_report_bits();
    pointing_button_manager_.reset();
    if (bits) {
      virtual_hid_device_client_.reset_virtual_hid_pointing();
    }
  }

  void set_system_preferences_values(const system_preferences::values& values) {
    std::lock_guard<std::mutex> guard(system_preferences_values_mutex_);

    system_preferences_values_ = values;
  }

  void clear_simple_modifications(void) {
    simple_modifications_.clear();
  }

  void add_simple_modification(krbn::key_code from_key_code, krbn::key_code to_key_code) {
    simple_modifications_.add(from_key_code, to_key_code);
  }

  void clear_fn_function_keys(void) {
    fn_function_keys_.clear();
  }

  void add_fn_function_key(krbn::key_code from_key_code, krbn::key_code to_key_code) {
    fn_function_keys_.add(from_key_code, to_key_code);
  }

  void initialize_virtual_hid_keyboard(const krbn::virtual_hid_keyboard_configuration_struct& configuration) {
    virtual_hid_device_client_.initialize_virtual_hid_keyboard(configuration);
  }

  void initialize_virtual_hid_pointing(void) {
    virtual_hid_device_client_.initialize_virtual_hid_pointing();
  }

  void terminate_virtual_hid_pointing(void) {
    virtual_hid_device_client_.terminate_virtual_hid_pointing();
  }

  void set_caps_lock_state(bool state) {
    modifier_flag_manager_.manipulate(krbn::modifier_flag::caps_lock,
                                      state ? modifier_flag_manager::operation::lock : modifier_flag_manager::operation::unlock);
  }

  void handle_keyboard_event(device_registry_entry_id device_registry_entry_id,
                             uint64_t timestamp,
                             krbn::key_code from_key_code,
                             bool pressed) {
    krbn::key_code to_key_code = from_key_code;

    // ----------------------------------------
    // process coder layout
    if (process_coder_layout(to_key_code, pressed)) {
      return;
    }

    // ----------------------------------------
    // modify keys
    if (!pressed) {
      if (auto key_code = manipulated_keys_.find(device_registry_entry_id, from_key_code)) {
        manipulated_keys_.remove(device_registry_entry_id, from_key_code);
        to_key_code = *key_code;
      }
    } else {
      if (auto key_code = simple_modifications_.get(from_key_code)) {
        manipulated_keys_.add(device_registry_entry_id, from_key_code, *key_code);
        to_key_code = *key_code;
      }
    }

    // ----------------------------------------
    // modify fn+arrow, function keys
    if (!pressed) {
      if (auto key_code = manipulated_fn_keys_.find(device_registry_entry_id, to_key_code)) {
        manipulated_fn_keys_.remove(device_registry_entry_id, to_key_code);
        to_key_code = *key_code;
      }
    } else {
      boost::optional<krbn::key_code> key_code;

      if (modifier_flag_manager_.pressed(krbn::modifier_flag::fn)) {
        switch (to_key_code) {
        case krbn::key_code::return_or_enter:
          key_code = krbn::key_code::keypad_enter;
          break;
        case krbn::key_code::delete_or_backspace:
          key_code = krbn::key_code::delete_forward;
          break;
        case krbn::key_code::right_arrow:
          key_code = krbn::key_code::end;
          break;
        case krbn::key_code::left_arrow:
          key_code = krbn::key_code::home;
          break;
        case krbn::key_code::down_arrow:
          key_code = krbn::key_code::page_down;
          break;
        case krbn::key_code::up_arrow:
          key_code = krbn::key_code::page_up;
          break;
        default:
          break;
        }
      }

      // f1-f12
      {
        auto key_code_value = static_cast<uint32_t>(to_key_code);
        if (kHIDUsage_KeyboardF1 <= key_code_value && key_code_value <= kHIDUsage_KeyboardF12) {
          bool keyboard_fn_state = false;
          {
            std::lock_guard<std::mutex> guard(system_preferences_values_mutex_);
            keyboard_fn_state = system_preferences_values_.get_keyboard_fn_state();
          }

          bool fn_pressed = modifier_flag_manager_.pressed(krbn::modifier_flag::fn);

          if ((fn_pressed && keyboard_fn_state) ||
              (!fn_pressed && !keyboard_fn_state)) {
            // change f1-f12 keys to media controls
            if (auto k = fn_function_keys_.get(to_key_code)) {
              key_code = *k;
            }
          }
        }
      }

      if (key_code) {
        manipulated_fn_keys_.add(device_registry_entry_id, to_key_code, *key_code);
        to_key_code = *key_code;
      }
    }

    // ----------------------------------------
    if (post_modifier_flag_event(to_key_code, pressed, timestamp)) {
      return;
    }

    post_key(to_key_code, pressed, timestamp);
  }

  void handle_pointing_event(device_registry_entry_id device_registry_entry_id,
                             uint64_t timestamp,
                             krbn::pointing_event pointing_event,
                             boost::optional<krbn::pointing_button> pointing_button,
                             CFIndex integer_value) {
    pqrs::karabiner_virtual_hid_device::hid_report::pointing_input report;

    switch (pointing_event) {
    case krbn::pointing_event::button:
      if (pointing_button && *pointing_button != krbn::pointing_button::zero) {
        pointing_button_manager_.manipulate(*pointing_button,
                                            integer_value ? pointing_button_manager::operation::increase : pointing_button_manager::operation::decrease);
      }
      break;

    case krbn::pointing_event::x:
      report.x = integer_value;
      break;

    case krbn::pointing_event::y:
      report.y = integer_value;
      break;

    case krbn::pointing_event::vertical_wheel:
      report.vertical_wheel = integer_value;
      break;

    case krbn::pointing_event::horizontal_wheel:
      report.horizontal_wheel = integer_value;
      break;

    default:
      break;
    }

    auto bits = pointing_button_manager_.get_hid_report_bits();
    report.buttons[0] = (bits >> 0) & 0xff;
    report.buttons[1] = (bits >> 8) & 0xff;
    report.buttons[2] = (bits >> 16) & 0xff;
    report.buttons[3] = (bits >> 24) & 0xff;
    virtual_hid_device_client_.post_pointing_input_report(report);
  }

  void stop_key_repeat(void) {
    virtual_hid_device_client_.reset_virtual_hid_keyboard();
  }

private:
  class manipulated_keys final {
  public:
    manipulated_keys(const manipulated_keys&) = delete;

    manipulated_keys(void) {
    }

    void clear(void) {
      std::lock_guard<std::mutex> guard(mutex_);

      manipulated_keys_.clear();
    }

    void add(device_registry_entry_id device_registry_entry_id,
             krbn::key_code from_key_code,
             krbn::key_code to_key_code) {
      std::lock_guard<std::mutex> guard(mutex_);

      manipulated_keys_.push_back(manipulated_key(device_registry_entry_id, from_key_code, to_key_code));
    }

    boost::optional<krbn::key_code> find(device_registry_entry_id device_registry_entry_id,
                                         krbn::key_code from_key_code) {
      std::lock_guard<std::mutex> guard(mutex_);

      for (const auto& v : manipulated_keys_) {
        if (v.get_device_registry_entry_id() == device_registry_entry_id &&
            v.get_from_key_code() == from_key_code) {
          return v.get_to_key_code();
        }
      }
      return boost::none;
    }

    void remove(device_registry_entry_id device_registry_entry_id,
                krbn::key_code from_key_code) {
      std::lock_guard<std::mutex> guard(mutex_);

      manipulated_keys_.remove_if([&](const manipulated_key& v) {
        return v.get_device_registry_entry_id() == device_registry_entry_id &&
               v.get_from_key_code() == from_key_code;
      });
    }

  private:
    class manipulated_key final {
    public:
      manipulated_key(device_registry_entry_id device_registry_entry_id,
                      krbn::key_code from_key_code,
                      krbn::key_code to_key_code) : device_registry_entry_id_(device_registry_entry_id),
                                                    from_key_code_(from_key_code),
                                                    to_key_code_(to_key_code) {
      }

      device_registry_entry_id get_device_registry_entry_id(void) const { return device_registry_entry_id_; }
      krbn::key_code get_from_key_code(void) const { return from_key_code_; }
      krbn::key_code get_to_key_code(void) const { return to_key_code_; }

    private:
      device_registry_entry_id device_registry_entry_id_;
      krbn::key_code from_key_code_;
      krbn::key_code to_key_code_;
    };

    std::list<manipulated_key> manipulated_keys_;
    std::mutex mutex_;
  };

  class simple_modifications final {
  public:
    simple_modifications(const simple_modifications&) = delete;

    simple_modifications(void) {
    }

    void clear(void) {
      std::lock_guard<std::mutex> guard(mutex_);

      map_.clear();
    }

    void add(krbn::key_code from_key_code, krbn::key_code to_key_code) {
      std::lock_guard<std::mutex> guard(mutex_);

      map_[from_key_code] = to_key_code;
    }

    boost::optional<krbn::key_code> get(krbn::key_code from_key_code) {
      std::lock_guard<std::mutex> guard(mutex_);

      auto it = map_.find(from_key_code);
      if (it != map_.end()) {
        return it->second;
      }

      return boost::none;
    }

  private:
    std::unordered_map<krbn::key_code, krbn::key_code> map_;
    std::mutex mutex_;
  };



  // CODER LAYOUT
  // -------------------
  bool MOD_APP_SWITCH = false;
  bool MOD_SPACEBAR = false;
  bool app_switch_triggered = false;
  std::unique_ptr<gcd_utility::main_queue_timer> spacebar_timer_;

  void post_key(std::string key_name) {
    post_key(key_name, true);
    post_key(key_name, false);
  }

  void post_key(std::string key_name, bool pressed) {
    if (auto key = krbn::types::get_key_code(key_name)) {
      if (!post_modifier_flag_event(*key, pressed)) {
        post_key(*key, pressed);
      }
    } else {
      logger::get_logger().warn("invalid key: {1}", key_name);
    }
  }

  bool is_key(krbn::key_code key_code, std::string key_name) {
    auto key_code_by_name = krbn::types::get_key_code(key_name);
    return key_code_by_name == key_code;
  }

  bool process_coder_layout(krbn::key_code key_code, bool pressed) {

    // Keep track of space bar state
    if (is_key(key_code, "spacebar")) {
      MOD_SPACEBAR = pressed;
    }

    // APP SWITCHING
    // --------------------
    //
    // 4 = most recent app
    // 4 + 3 = backward in apps
    // 4 + r = forward in apps
    //
    // Requires app-switching to be mapped to cmd-tab
    //

    // Start app/window switch mode by pressing "4".
    if (is_key(key_code, "4") && !MOD_SPACEBAR) {
      MOD_APP_SWITCH = pressed;
      if (pressed) {
        post_key("left_command", true);
      } else {
        if (!app_switch_triggered) {
          post_key("tab");
        }
        MOD_APP_SWITCH = false;
        app_switch_triggered = false;
        post_key("left_command", false);
      }
      return true;
    }

    if (MOD_APP_SWITCH) {
      // Previous app by pressing "3".
      if (is_key(key_code, "3")) {
        if (pressed) {
          app_switch_triggered = true;
          post_key("left_shift", true);
          post_key("tab");
          post_key("left_shift", false);
        }
      }
      // Next app by pressing "r".
      if (is_key(key_code, "r")) {
        if (pressed) {
          app_switch_triggered = true;
          post_key("tab");
        }
      }
      return true;
    }

    // WINDOW SWITCHING
    // --------------------
    //
    // space + 4 = next window
    // space + 3 = previous window
    //
    // Requires window-switching to be mapped to ctrl-cmd-tab
    //

    // next window by pressing "4".
    if (is_key(key_code, "4") && MOD_SPACEBAR) {
      if (pressed) {
        post_key("left_command", true);
        post_key("left_control", true);
        post_key("tab");
        post_key("left_control", false);
        post_key("left_command", false);
      }
      return true;
    }

    // previous window by pressing "3".
    if (is_key(key_code, "3") && MOD_SPACEBAR) {
      if (pressed) {
        post_key("left_command", true);
        post_key("left_control", true);
        post_key("left_shift", true);
        post_key("tab");
        post_key("left_shift", false);
        post_key("left_control", false);
        post_key("left_command", false);
      }
      return true;
    }

    // SPACEBAR as SHIFT
    // ----------------------
    //
    // Keeping space bar pressed will make it act as shift key.
    //
    if (pressed) {
      if (is_key(key_code, "spacebar")) {
        // After 200ms we will set shift modifier flag unless spacebar key
        // got released or another key was pressed before.
        long spacebar_milliseconds = 200;
        spacebar_timer_ = std::make_unique<gcd_utility::main_queue_timer>(
            DISPATCH_TIMER_STRICT,
            dispatch_time(DISPATCH_TIME_NOW, spacebar_milliseconds * NSEC_PER_MSEC),
            spacebar_milliseconds * NSEC_PER_MSEC,
            0,
            ^{
              post_key("left_shift", true);
              spacebar_timer_ = nullptr;
            });
        return true;
      } else {
        // Another key was pressed while spacebar is pressed.
        // Activate shift.
        if (spacebar_timer_ != nullptr) {
          post_key("left_shift", true);
          spacebar_timer_ = nullptr;
        }
      }
    } else {
      if (is_key(key_code, "spacebar")) {
        // If spacebar was released sooner than 200ms and no other key was
        // pressed meanwhile, then post a space
        if (spacebar_timer_ != nullptr) {
          spacebar_timer_ = nullptr;
          post_key("spacebar");
          return true;
        } else {
          post_key("left_shift", false);
          return true;
        }
      }
    }


    // NAVIGATION LAYER
    // ----------------------
    //
    // Pressing Caps-Lock or quote activates navigation layer.
    //

    return false;
  }

  void reset_coder_layout(void) {
    MOD_APP_SWITCH = false;
    MOD_SPACEBAR = false;
    app_switch_triggered = false;
    spacebar_timer_ = nullptr;
  }




  bool post_modifier_flag_event(krbn::key_code key_code, bool pressed, uint64_t timestamp) {
    auto operation = pressed ? manipulator::modifier_flag_manager::operation::increase : manipulator::modifier_flag_manager::operation::decrease;

    auto modifier_flag = krbn::types::get_modifier_flag(key_code);
    if (modifier_flag != krbn::modifier_flag::zero) {
      modifier_flag_manager_.manipulate(modifier_flag, operation);

      post_key(key_code, pressed, timestamp);
      return true;
    }

    return false;
  }

  void post_key(krbn::key_code key_code, bool pressed, uint64_t timestamp) {
    add_delay_to_continuous_event(timestamp);

    if (auto usage_page = krbn::types::get_usage_page(key_code)) {
      if (auto usage = krbn::types::get_usage(key_code)) {
        pqrs::karabiner_virtual_hid_device::hid_event_service::keyboard_event keyboard_event;
        keyboard_event.usage_page = *usage_page;
        keyboard_event.usage = *usage;
        keyboard_event.value = pressed;
        virtual_hid_device_client_.dispatch_keyboard_event(keyboard_event);
      }
    }
  }

  void add_delay_to_continuous_event(uint64_t timestamp) {
    if (timestamp != last_timestamp_) {
      last_timestamp_ = timestamp;

    } else {
      // We need to add a delay to continous events to ensure the key events order in WindowServer.
      //
      // Unless the delay, application will receive FlagsChanged event after KeyDown events even if the modifier key is sent before.
      //
      // Example of no delay:
      //   In event_manipulator:
      //     1. send shift key down
      //     2. send tab key down
      //     3. send tab key up
      //     4. send shift key up
      //
      //   In application
      //     1. KeyDown tab
      //     2. FlagsChanged shift
      //     3. KeyUp tab
      //     4. FlagsChanged shift
      //
      // We need the delay to avoid this order changes.

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  virtual_hid_device_client& virtual_hid_device_client_;
  modifier_flag_manager modifier_flag_manager_;
  pointing_button_manager pointing_button_manager_;

  system_preferences::values system_preferences_values_;
  std::mutex system_preferences_values_mutex_;

  simple_modifications simple_modifications_;
  simple_modifications fn_function_keys_;

  manipulated_keys manipulated_keys_;
  manipulated_keys manipulated_fn_keys_;

  uint64_t last_timestamp_;
};
}
