#include "core/ble_midi.h"

#include <systemd/sd-bus.h>

#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cstdint>

namespace nirbija {
namespace {

constexpr char kMidiIoUuid[] = "7772e5db-3868-4112-a1a9-f2669d106bf3";

int data_bytes(uint8_t status) {
  const uint8_t high = status & 0xf0;
  if (high == 0xc0 || high == 0xd0) return 1;
  if (status >= 0xf8) return 0;
  return 2;
}

void lower_uuid(char* dest, const char* src, size_t cap) {
  size_t i = 0;
  for (; src[i] != 0 && i + 1 < cap; ++i)
    dest[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(src[i])));
  dest[i] = 0;
}

}  // namespace

BleMidi::BleMidi() = default;

BleMidi::~BleMidi() { stop(); }

void BleMidi::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { thread_main(); });
}

void BleMidi::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

size_t BleMidi::pop(MidiEvent* out, size_t capacity) {
  size_t n = 0;
  MidiEvent event;
  while (n < capacity && queue_.pop(event)) out[n++] = event;
  return n;
}

void BleMidi::emit(uint8_t status, uint8_t d1, uint8_t d2, uint8_t size) {
  MidiEvent event;
  event.frame = 0;
  event.size = size;
  event.data[0] = status;
  event.data[1] = d1;
  event.data[2] = d2;
  queue_.push(event);
}

void BleMidi::feed(const uint8_t* data, size_t n) {
  // BLE MIDI: header (bit 7 set), then timestamp (bit 7 set) + MIDI.
  // Realtime (0xF8–0xFF) may sit anywhere. See MMA BLE MIDI spec.
  if (n < 2) return;
  size_t i = (data[0] & 0x80) ? 1 : 0;
  while (i < n) {
    uint8_t byte = data[i];
    if (byte >= 0xf8) {
      emit(byte, 0, 0, 1);
      ++i;
      continue;
    }
    if (byte & 0x80) {
      // Timestamp. Next is status, or data under running status.
      ++i;
      if (i >= n) break;
      byte = data[i];
      if (byte >= 0xf8) {
        emit(byte, 0, 0, 1);
        ++i;
        continue;
      }
      if (byte & 0x80) {
        running_status_ = byte;
        ++i;
        if (i >= n) break;
        byte = data[i];
      }
    }
    if (running_status_ < 0x80) {
      ++i;
      continue;
    }
    const int need = data_bytes(running_status_);
    const uint8_t d1 = byte;
    uint8_t d2 = 0;
    ++i;
    if (need == 2) {
      if (i >= n) break;
      if (data[i] & 0x80) continue;
      d2 = data[i++];
    }
    emit(running_status_, d1, d2, static_cast<uint8_t>(need + 1));
  }
}

std::string find_midi_char(sd_bus* bus) {
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  int rc = sd_bus_call_method(bus, "org.bluez", "/",
                              "org.freedesktop.DBus.ObjectManager",
                              "GetManagedObjects", &error, &reply, "");
  if (rc < 0) {
    sd_bus_error_free(&error);
    return {};
  }

  std::string found;
  sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
  while (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
    const char* path = nullptr;
    sd_bus_message_read(reply, "o", &path);
    sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
    while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
      const char* iface = nullptr;
      sd_bus_message_read(reply, "s", &iface);
      const bool want =
          iface != nullptr &&
          std::strcmp(iface, "org.bluez.GattCharacteristic1") == 0;
      if (!want) {
        sd_bus_message_skip(reply, "a{sv}");
        sd_bus_message_exit_container(reply);
        continue;
      }
      sd_bus_message_enter_container(reply, 'a', "{sv}");
      while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char* key = nullptr;
        sd_bus_message_read(reply, "s", &key);
        if (key != nullptr && std::strcmp(key, "UUID") == 0) {
          const char* uuid = nullptr;
          sd_bus_message_enter_container(reply, 'v', "s");
          sd_bus_message_read(reply, "s", &uuid);
          char lowered[80];
          if (uuid != nullptr) {
            lower_uuid(lowered, uuid, sizeof(lowered));
            if (std::strcmp(lowered, kMidiIoUuid) == 0 && path != nullptr)
              found = path;
          }
          sd_bus_message_exit_container(reply);
        } else {
          sd_bus_message_skip(reply, "v");
        }
        sd_bus_message_exit_container(reply);
      }
      sd_bus_message_exit_container(reply);
      sd_bus_message_exit_container(reply);
    }
    sd_bus_message_exit_container(reply);
    sd_bus_message_exit_container(reply);
  }
  sd_bus_message_unref(reply);
  sd_bus_error_free(&error);
  return found;
}

bool BleMidi::run_one_device() {
  sd_bus* bus = nullptr;
  if (sd_bus_open_system(&bus) < 0) return false;
  const std::string path = find_midi_char(bus);
  if (path.empty()) {
    sd_bus_unref(bus);
    return false;
  }

  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  int rc = sd_bus_call_method(bus, "org.bluez", path.c_str(),
                              "org.bluez.GattCharacteristic1", "AcquireNotify",
                              &error, &reply, "a{sv}", 0);
  if (rc < 0) {
    sd_bus_error_free(&error);
    sd_bus_unref(bus);
    return false;
  }

  int fd = -1;
  uint16_t mtu = 0;
  rc = sd_bus_message_read(reply, "hq", &fd, &mtu);
  // The FD in the message is closed when the message is unref'd. Dup first.
  const int owned = (rc >= 0 && fd >= 0) ? dup(fd) : -1;
  sd_bus_message_unref(reply);
  if (owned < 0) {
    sd_bus_unref(bus);
    return false;
  }
  fcntl(owned, F_SETFL, O_NONBLOCK);

  uint8_t buffer[512];
  while (running_.load(std::memory_order_relaxed)) {
    pollfd pfd{owned, POLLIN, 0};
    const int pr = poll(&pfd, 1, 250);
    if (pr == 0) continue;
    if (pr < 0) break;
    const ssize_t n = read(owned, buffer, sizeof(buffer));
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    feed(buffer, static_cast<size_t>(n));
  }

  close(owned);
  sd_bus_unref(bus);
  return true;
}

void BleMidi::thread_main() {
  while (running_.load(std::memory_order_relaxed)) {
    if (!run_one_device())
      usleep(1500 * 1000);
  }
}

}  // namespace nirbija
