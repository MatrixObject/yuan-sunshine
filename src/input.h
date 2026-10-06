/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// local includes
#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param input Raw input packet to format for logging.
   */
  void print(void *input);
  /**
   * @brief Reset stream input state after a client disconnect or shutdown.
   *
   * @param input Shared stream input state to reset.
   */
  void reset(std::shared_ptr<input_t> &input);

  /**
   * @brief Destroy every retained virtual gamepad session.
   *
   * Retained gamepads survive a paused transport connection so they can be reused on resume. Call this when the
   * streamed application or all streaming sessions are explicitly terminated.
   */
  void terminate_gamepads();

  /**
   * @brief Destroy virtual gamepads retained for one paired client.
   *
   * @param session_id Stable paired-client identity used by alloc().
   */
  void terminate_gamepads(std::string_view session_id);

  /**
   * @brief Queue a raw input message for platform passthrough.
   */
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data);

  /**
   * @brief Initialize global input resources and platform backends.
   *
   * @return Cleanup handle for initialized input resources, or null if none are required.
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Probe whether the platform can create virtual gamepads.
   *
   * @return True when at least one configured gamepad backend is available.
   */
  bool probe_gamepads();

  /**
   * @brief Recreate shared libvirtualhid keyboard and mouse devices after a license-state change.
   *
   * The work is serialized with streamed input so both backends can switch
   * safely between the Windows HID and SendInput paths.
   */
  void refresh_virtual_input();

  /**
   * @brief Allocate and initialize platform input state for a stream.
   *
   * @param mail Mailbox used to exchange messages with worker threads.
   * @param session_id Stable paired-client identity shared by launch and resume connections.
   * @return Shared input state bound to the stream mailbox.
   */
  std::shared_ptr<input_t> alloc(safe::mail_t mail, std::string session_id);

#ifdef SUNSHINE_TESTS
  namespace testing {
    /**
     * @brief Replace the global platform input backend for a unit test.
     *
     * @param input Test-owned platform input backend.
     */
    void set_platform_input(platf::input_t input);

    /**
     * @brief Allocate a gamepad directly in retained input state for a unit test.
     *
     * @param input Retained input state.
     * @param client_index Client-relative controller index.
     * @param metadata Client-reported controller metadata.
     * @return Assigned global gamepad slot, or -1 on failure.
     */
    int alloc_gamepad(std::shared_ptr<input_t> &input, std::uint8_t client_index, const platf::gamepad_arrival_t &metadata);

    /**
     * @brief Return the global gamepad slot stored for a test controller.
     *
     * @param input Retained input state.
     * @param client_index Client-relative controller index.
     * @return Assigned global gamepad slot, or -1 when unallocated.
     */
    int gamepad_id(const std::shared_ptr<input_t> &input, std::uint8_t client_index);

    /**
     * @brief Keyboard event Sunshine emitted toward the platform backend.
     */
    struct keyboard_event_t {
      std::uint16_t key_code;  ///< Platform keycode after the configured keybinding remap.
      bool release;  ///< Whether the event releases the key.
      std::uint8_t flags;  ///< Bit flags carried by the client keyboard packet.
    };

    /**
     * @brief Redirect keyboard output away from the host operating system.
     *
     * Tests must install a sink before emitting keys, otherwise the events are typed into the
     * machine running the test suite.
     *
     * @param sink Recorder invoked in place of platf::keyboard_update, or empty to restore
     *             delivery to the platform backend.
     */
    void set_keyboard_sink(std::function<void(const keyboard_event_t &)> sink);

    /**
     * @brief Process one client keyboard packet on the calling thread.
     *
     * @param input Retained input state.
     * @param key_code Windows virtual-key code sent by the client.
     * @param modifiers Client modifier bitmask carried by the packet.
     * @param flags Bit flags carried by the client keyboard packet.
     * @param release Whether the packet releases the key.
     */
    void send_keyboard_packet(std::shared_ptr<input_t> &input, std::uint16_t key_code, std::uint8_t modifiers, std::uint8_t flags, bool release);

    /**
     * @brief Forget every key Sunshine tracks as pressed and cancel any pending key repeat.
     */
    void reset_keyboard_state();

    /**
     * @brief Release every key Sunshine tracks as pressed, as a disconnect does.
     */
    void release_held_keys();

    /**
     * @brief Validate raw protocol input bytes for a unit test.
     *
     * @param packet Raw packet bytes.
     * @return True when the packet is safe for typed processing.
     */
    bool is_valid_input_packet(std::span<const std::uint8_t> packet);

    /**
     * @brief Return the number of validated packets waiting in a test input queue.
     *
     * @param input Shared stream input state.
     * @return Number of queued packets, or zero for an empty input pointer.
     */
    std::size_t queued_input_packet_count(const std::shared_ptr<input_t> &input);

    /**
     * @brief Determine whether window capture mode is currently active.
     *
     * @return True when a capture process or capture window is configured.
     */
    bool is_window_capture_mode();

    /**
     * @brief Determine whether an input packet originates from a gamepad.
     *
     * @param magic Little-endian input packet magic value.
     * @return True when the packet carries gamepad control data.
     */
    bool is_gamepad_input(std::uint32_t magic);
  }  // namespace testing
#endif

  /**
   * @brief Touchscreen coordinate bounds used to scale absolute input.
   */
  struct touch_port_t: public platf::touch_port_t {
    int env_width;  ///< Width of the full capture environment in physical pixels.
    int env_height;  ///< Height of the full capture environment in physical pixels.

    // Offset x and y coordinates of the client
    float client_offsetX;  ///< Horizontal client viewport offset used when scaling touch input.
    float client_offsetY;  ///< Vertical client viewport offset used when scaling touch input.

    float scalar_inv;  ///< Inverse scale factor from client coordinates to display coordinates.
    float scalar_tpcoords;  ///< Scale factor from client coordinates to touch-port coordinates.

    int env_logical_width;  ///< Width of the full capture environment after display scaling.
    int env_logical_height;  ///< Height of the full capture environment after display scaling.

    /**
     * @brief Check whether the touch-port bounds are initialized.
     */
    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);

  /**
   * @brief Destination for XInput-over-UDP gamepad delivery.
   *
   * The injected game process rewrites XInputGetState from loopback UDP
   * snapshots; the virtual pad stays attached, so this only mirrors client
   * state to the game itself.
   */
  enum class xinput_delivery {
    off,  ///< No UDP delivery; the virtual pad is the only gamepad sink.
    global,  ///< Deliver to the fixed loopback base port.
    process,  ///< Deliver to the injected game process via the fixed loopback port.
  };

  /**
   * @brief Mirror of the Windows XINPUT_GAMEPAD wire layout (little-endian).
   */
  struct xinput_gamepad_t {
    std::uint16_t buttons;  ///< XINPUT button mask.
    std::uint8_t left_trigger;  ///< Left trigger 0-255.
    std::uint8_t right_trigger;  ///< Right trigger 0-255.
    std::int16_t thumb_lx;  ///< Left stick X axis.
    std::int16_t thumb_ly;  ///< Left stick Y axis.
    std::int16_t thumb_rx;  ///< Right stick X axis.
    std::int16_t thumb_ry;  ///< Right stick Y axis.
  };

  #pragma pack(push, 1)
  /**
   * @brief One gamepad snapshot delivered to the injected process over loopback UDP.
   */
  struct xinput_udp_packet {
    std::uint32_t magic;  ///< Identifies snapshots; equals xinput_udp_magic.
    std::uint32_t index;  ///< XInput slot the snapshot targets.
    std::uint32_t packet_number;  ///< Monotonic host counter.
    xinput_gamepad_t gamepad;  ///< Button / trigger / stick state.
  };
  #pragma pack(pop)

  static_assert(sizeof(xinput_gamepad_t) == 12, "XINPUT_GAMEPAD wire size");
  static_assert(sizeof(xinput_udp_packet) == 24, "XInput UDP wire size");

  /**
   * @brief Default loopback base port for global XInput delivery.
   */
  constexpr std::uint16_t xinput_udp_default_port = 45690;

  /**
   * @brief Datagram magic identifying an XInput snapshot ("XIP1" little-endian).
   */
  constexpr std::uint32_t xinput_udp_magic = 0x31504958u;

  /**
   * @brief Encode a Sunshine gamepad state as a raw XInput snapshot.
   *
   * @param gamepad_state Client gamepad button and axis state.
   * @param index XInput slot to stamp into the packet.
   * @param packet_number Monotonic host counter for the caller's slot.
   * @return The wire packet ready to send over loopback UDP.
   */
  xinput_udp_packet make_xinput_packet(const platf::gamepad_state_t &gamepad_state, std::uint32_t index, std::uint32_t packet_number);

  /**
   * @brief Determine whether a gamepad snapshot carries any input at all.
   *
   * Moonlight clients report a pad only when it changes, so a held stick or button
   * produces silence on the wire. Snapshots that carry input must be republished by
   * the host until the client reports a new state, otherwise the injected XInput
   * rewrite expires the slot and the game falls back to an idle pad. An all-zero
   * snapshot needs no republishing: it is exactly what the idle virtual pad reports.
   *
   * @param gamepad_state Client gamepad button and axis state.
   * @return True when a button, trigger, or stick is away from its rest position.
   */
  bool gamepad_state_held(const platf::gamepad_state_t &gamepad_state);

  /**
   * @brief Set the runtime XInput-over-UDP delivery mode and base port.
   *
   * The values are runtime-only and are never persisted to sunshine.conf.
   * Both delivery modes use the same fixed loopback base port; the
   * per-process aspect comes from the injected game being the only listener,
   * never from a PID-derived destination.
   *
   * @param mode Target delivery mode.
   * @param base_port Loopback base port used in global and process modes.
   */
  void set_xinput_delivery(xinput_delivery mode, std::uint16_t base_port = xinput_udp_default_port);

  /**
   * @brief Query the current XInput-over-UDP delivery mode.
   * @return The active delivery mode.
   */
  xinput_delivery get_xinput_delivery();

  /**
   * @brief Query the current XInput-over-UDP base port.
   * @return The active base port.
   */
  std::uint16_t get_xinput_udp_base_port();
}  // namespace input
