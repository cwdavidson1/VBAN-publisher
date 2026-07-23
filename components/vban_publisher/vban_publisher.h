#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/sensor/sensor.h"

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include <lwip/inet.h>
#include <lwip/sockets.h>

/* Tunables */
// #define VBAN_SAMPLES_PER_PACKET 128
#define VBAN_SAMPLES_PER_PACKET 256
#define VBAN_RING_PACKETS 32
#define VBAN_TASK_STACK 4096
#define VBAN_TASK_PRIORITY 4

/* VBAN header (packed, compliant, with frame counter) */
struct __attribute__((packed)) VBANHeader {
    char vban[4];           // "VBAN"
    uint8_t format_sr;      // sample-rate index
    uint8_t format_nbs;     // samples per frame - 1
    uint8_t format_nbc;     // channels - 1
    uint8_t format_format;  // format / codec
    char streamname[16];    // null padded
    uint32_t frame_counter; // REQUIRED
};

struct AudioPacket {
    int16_t samples[VBAN_SAMPLES_PER_PACKET];
};

namespace esphome::vban_publisher {

class VBANPublisherComponent final : public Component {
  public:
    void dump_config() override;
    void setup() override;
    void loop() override;

    float get_setup_priority() const override {
        return setup_priority::AFTER_CONNECTION;
    }

    void set_measurement_duration(uint32_t measurement_duration_ms) {
        this->measurement_duration_ms_ = measurement_duration_ms;
    }
    void
    set_microphone_source(microphone::MicrophoneSource *microphone_source) {
        this->microphone_source_ = microphone_source;
    }
    void set_peak_sensor(sensor::Sensor *peak_sensor) {
        this->peak_sensor_ = peak_sensor;
    }
    void set_rms_sensor(sensor::Sensor *rms_sensor) {
        this->rms_sensor_ = rms_sensor;
    }

    void set_target_ip(const std::string &ip) { this->target_ip_str_ = ip; }
    void set_target_port(uint16_t port) { this->port_ = port; }
    void set_sample_rate(uint32_t rate) { this->sample_rate_ = rate; }
    void set_stream_name(const std::string &name) {
        this->stream_name_ = name.substr(0, 16);
    }
    void set_gain(float gain) {
        if (gain < 0.0f)
            gain = 0.0f;
        if (gain > 10.0f)
            gain = 10.0f;
        this->gain_ = gain;
    }

    /// @brief Starts the MicrophoneSource to start measuring sound levels
    void start();

    /// @brief Stops the MicrophoneSource
    void stop();

  protected:
    /// @brief Internal start command that, if necessary, allocates a ring
    /// buffer and a zero-copy
    /// ``RingBufferAudioSource`` that reads directly from it. ``ring_buffer_``
    /// weakly references the ring buffer owned by ``audio_source_``. Returns
    /// true if allocations were successful.
    bool start_();

    /// @brief Internal stop command that deallocates ``audio_source_`` (which
    /// releases its ring buffer)
    void stop_();
    uint8_t vban_sr_index_(uint32_t rate);

    microphone::MicrophoneSource *microphone_source_{nullptr};

    sensor::Sensor *peak_sensor_{nullptr};
    sensor::Sensor *rms_sensor_{nullptr};

    std::unique_ptr<audio::RingBufferAudioSource> audio_source_;
    std::weak_ptr<ring_buffer::RingBuffer> ring_buffer_;

    int32_t squared_peak_{0};
    uint64_t squared_samples_sum_{0};
    uint32_t sample_count_{0};

    uint32_t measurement_duration_ms_;

    int sock_{-1};
    sockaddr_in dest_addr_{};

    std::string target_ip_str_;
    uint16_t port_{6980};
    uint32_t sample_rate_{48000};
    std::string stream_name_{"ESP32"};

    float gain_{1.0f}; // digital mic gain

    AudioPacket pkt_;
    uint8_t packet_[sizeof(VBANHeader) + sizeof(AudioPacket)];
    uint32_t frame_counter_ = 0;
};

template <typename... Ts>
class StartAction final : public Action<Ts...>,
                          public Parented<VBANPublisherComponent> {
  public:
    void play(const Ts &...x) override { this->parent_->start(); }
};

template <typename... Ts>
class StopAction final : public Action<Ts...>,
                         public Parented<VBANPublisherComponent> {
  public:
    void play(const Ts &...x) override { this->parent_->stop(); }
};

} // namespace esphome::vban_publisher

#endif
