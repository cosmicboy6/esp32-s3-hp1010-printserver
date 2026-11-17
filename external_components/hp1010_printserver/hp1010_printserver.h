#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/usb_host/usb_host.h"
#include "usb/usb_host.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace esphome::hp1010_printserver {

// Transparent raw-9100 (JetDirect/AppSocket) -> USB Printer Class bridge.
//
// The printer is a host-based HP LaserJet 1010: the client driver renders the
// job and sends the ready-to-print byte stream to TCP port 9100. This component
// only forwards those bytes to the printer's bulk OUT endpoint; it does not
// interpret PCL/GDI data.
class HP1010PrintServer : public usb_host::USBClient {
 public:
  HP1010PrintServer(uint16_t vid, uint16_t pid) : usb_host::USBClient(vid, pid) {}

  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_port(uint16_t port) { this->port_ = port; }

  /// True while the printer is enumerated, claimed, and selected for raw data.
  bool is_ready() const { return this->printer_ready_.load(std::memory_order_acquire); }

 protected:
  void on_connected() override;
  void on_disconnected() override;

 private:
  // Bulk transfers are queued strictly one at a time: the ESP-IDF HCD rejects a
  // second concurrent URB on the same non-periodic endpoint with
  // ESP_ERR_INVALID_STATE, so MAX_INFLIGHT must stay 1. 4 KiB chunks match the
  // reference ZP450 print server and keep the submit/completion round trips low.
  static constexpr size_t CHUNK_SIZE = 4096;
  static constexpr size_t CHUNK_COUNT = 8;
  static constexpr size_t MAX_INFLIGHT = 1;

  struct Chunk {
    uint8_t data[CHUNK_SIZE];
    size_t len;
  };

  struct OutXferCtx {
    HP1010PrintServer *self;
    Chunk *chunk;
  };

  bool find_printer_interface_();
  void select_alt_setting_(uint8_t alt);
  void printer_class_port_status_();

  static void tcp_task_entry_(void *arg);
  void tcp_task_();
  void handle_client_(int fd);
  void start_output_();
  void start_input_();
  static void out_transfer_cb_(usb_transfer_t *xfer);

  uint16_t port_{9100};

  std::atomic<uint8_t> iface_{0};
  std::atomic<uint8_t> ep_out_{0};
  std::atomic<uint8_t> ep_in_{0};
  std::atomic<bool> claimed_{false};
  std::atomic<bool> printer_ready_{false};
  std::atomic<bool> status_inflight_{false};
  std::atomic<size_t> inflight_{0};
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint32_t> out_bytes_{0};
  uint16_t ep_out_mps_{64};
  uint16_t ep_in_mps_{64};

  Chunk chunks_[CHUNK_COUNT];
  QueueHandle_t free_queue_{nullptr};
  QueueHandle_t ready_queue_{nullptr};
  // Serializes the ready-queue check and the transfer submit so the byte stream
  // is never reordered or dropped when both the TCP task and the USB completion
  // callback try to feed the pipe at once.
  SemaphoreHandle_t out_mutex_{nullptr};
  TaskHandle_t tcp_task_handle_{nullptr};
  bool tcp_started_{false};
};

}  // namespace esphome::hp1010_printserver
