#include "vban_publisher.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <cmath>
#include <cstdint>

namespace esphome::vban_publisher {

static const char *const TAG = "vban_publisher";

static const uint32_t MAX_FILL_DURATION_MS = 30;
static const uint32_t RING_BUFFER_DURATION_MS = 120;

// Square INT16_MIN since INT16_MIN^2 > INT16_MAX^2
static const double MAX_SAMPLE_SQUARED_DENOMINATOR = INT16_MIN * INT16_MIN;

void VBANPublisherComponent::dump_config() {
    ESP_LOGCONFIG(TAG,
                  "VBAN Publisher Component:\n"
                  "  Measurement Duration: %" PRIu32 " ms",
                  measurement_duration_ms_);
    LOG_SENSOR("  ", "Peak:", this->peak_sensor_);

    LOG_SENSOR("  ", "RMS:", this->rms_sensor_);
}
/* VBAN sample-rate index mapping (16 kHz = index 8) */
uint8_t VBANPublisherComponent::vban_sr_index_(uint32_t rate) {
    switch (rate) {
    case 6000:
        return 0;
    case 12000:
        return 1;
    case 24000:
        return 2;
    case 48000:
        return 3;
    case 96000:
        return 4;
    case 192000:
        return 5;
    case 8000:
        return 7;
    case 16000:
        return 8;
    case 32000:
        return 9;
    case 64000:
        return 10;
    default:
        ESP_LOGW(TAG, "Unsupported sample rate %u, defaulting to 48000", rate);
        return 3;
    }
}
void VBANPublisherComponent::setup() {

    ESP_LOGI(TAG, "VBAN dest %s:%u", target_ip_str_.c_str(), port_);

    this->microphone_source_->add_data_callback(
        [this](const std::vector<uint8_t> &data) {
            std::shared_ptr<ring_buffer::RingBuffer> temp_ring_buffer =
                this->ring_buffer_.lock();
            if (temp_ring_buffer != nullptr) {
                temp_ring_buffer->write((void *)data.data(), data.size());
            }
        });

    if (!this->microphone_source_->is_passive()) {
        // Automatically start the microphone if not in passive mode
        this->microphone_source_->start();
    }
}

void VBANPublisherComponent::loop() {

    if (this->microphone_source_->is_running() && !this->status_has_error()) {
        // Allocate buffers
        if (this->start_()) {
            this->status_clear_warning();
        }
    } else {
        if (!this->status_has_warning()) {
            this->status_set_warning(
                LOG_STR("Microphone isn't running, can't compute statistics"));

            // Deallocate buffers, if necessary
            this->stop_();
        }

        return;
    }

    if (this->status_has_error()) {
        return;
    }

    // Expose a chunk of the ring buffer's internal storage - don't block to
    // avoid slowing the main loop. pre_shift is ignored by
    // RingBufferAudioSource (no intermediate transfer buffer to compact).
    this->audio_source_->fill(0, false);

    uint32_t samples_available_to_process =
        this->microphone_source_->get_audio_stream_info().bytes_to_samples(
            this->audio_source_->available());

    ESP_LOGD(TAG, "samples available: %d ", samples_available_to_process);

    if (samples_available_to_process < VBAN_16BIT_SAMPLES_PER_PACKET) {
        // Not enough new audio available for processing
        return;
    }

    const int32_t *raw_samples =
        reinterpret_cast<const int32_t *>(this->audio_source_->data());

    size_t count = samples_available_to_process;

    while (count >= VBAN_16BIT_SAMPLES_PER_PACKET) {

        // change 32 bit left justified input samples to 16 bit right justified
        for (size_t i = 0; i < VBAN_16BIT_SAMPLES_PER_PACKET; i++) {
            int32_t s = static_cast<int32_t>(raw_samples[i]);
            s >>= 16;
            if (s > 32767)
                s = 32767;
            if (s < -32768)
                s = -32768;
            pkt_.samples[i] = static_cast<int16_t>(s);
        }

        uint8_t datagram_payload[sizeof(VBANHeader) + sizeof(pkt_.samples)];

        // populate socket payload starting with header information
        VBANHeader *hdr = reinterpret_cast<VBANHeader *>(datagram_payload);
        memcpy(hdr->vban, "VBAN", 4);
        hdr->format_sr = vban_sr_index_(sample_rate_);
        hdr->format_nbs = VBAN_16BIT_SAMPLES_PER_PACKET - 1;
        hdr->format_nbc = 0;
        hdr->format_format = 0x01;
        memset(hdr->streamname, 0, sizeof(hdr->streamname));
        memcpy(hdr->streamname, stream_name_.c_str(), stream_name_.size());
        // update frame counter for next packet after setting in this header
        ESP_LOGD(TAG, "frame counter: %d ", frame_counter_);
        hdr->frame_counter = frame_counter_++;

        // copy in the audio samples after the header
        memcpy(datagram_payload + sizeof(VBANHeader), pkt_.samples,
               sizeof(pkt_.samples));

        int sockerr = sendto(sock_, datagram_payload, sizeof(datagram_payload),
                             0, reinterpret_cast<sockaddr *>(&dest_addr_),
                             sizeof(dest_addr_));

        if (sockerr < 0) {
            ESP_LOGE(TAG, "Socket Send error: %d", sockerr);
        }
        raw_samples += VBAN_16BIT_SAMPLES_PER_PACKET * 4;
        count -= VBAN_16BIT_SAMPLES_PER_PACKET;
    }

    this->audio_source_->consume(VBAN_16BIT_SAMPLES_PER_PACKET * 2);

    // const uint32_t samples_in_window =
    //     this->microphone_source_->get_audio_stream_info().ms_to_samples(this->measurement_duration_ms_);
    // const uint32_t samples_available_to_process =
    //     this->microphone_source_->get_audio_stream_info().bytes_to_samples(this->audio_source_->available());
    // const uint32_t samples_to_process = std::min(samples_in_window -
    // this->sample_count_, samples_available_to_process);

    // MicrophoneSource always provides int16 samples due to Python codegen
    // settings
    // const int16_t *audio_data = reinterpret_cast<const int16_t
    // *>(this->audio_source_->data());

    // Process all the new audio samples
    // for (uint32_t i = 0; i < samples_to_process; ++i) {
    //   // Squaring int16 samples won't overflow an int32
    //   int32_t squared_sample = static_cast<int32_t>(audio_data[i]) *
    //   static_cast<int32_t>(audio_data[i]);

    //   if (this->peak_sensor_ != nullptr) {
    //     this->squared_peak_ = std::max(this->squared_peak_,
    //     squared_sample);
    //   }

    //   if (this->rms_sensor_ != nullptr) {
    //     // Squared sum is an uint64 type - at max levels, an uint32 type
    //     would overflow after ~8 samples this->squared_samples_sum_ +=
    //     squared_sample;
    //   }

    // ++this->sample_count_;
    //}

    // Remove the processed samples from ``audio_source_``
    // this->audio_source_->consume(this->microphone_source_->get_audio_stream_info().samples_to_bytes(samples_to_process));
}

void VBANPublisherComponent::start() {
    if (this->microphone_source_->is_passive()) {
        ESP_LOGW(TAG, "Can't start the microphone in passive mode");
        return;
    }
    this->microphone_source_->start();
}

void VBANPublisherComponent::stop() {
    if (this->microphone_source_->is_passive()) {
        ESP_LOGW(TAG, "Can't stop microphone in passive mode");
        return;
    }
    this->microphone_source_->stop();
}

bool VBANPublisherComponent::start_() {
    if (this->audio_source_ != nullptr) {
        return true;
    }

    const auto &stream_info = this->microphone_source_->get_audio_stream_info();
    const size_t bytes_per_frame = stream_info.frames_to_bytes(1);

    // Allocate a ring buffer for the microphone callback to write into.
    // Round the size down to a multiple of bytes_per_frame so the wrap
    // boundary stays frame-aligned and avoids unnecessary single-frame
    // splices.
    this->ring_buffer_
        .reset(); // Reset pointer to any previous ring buffer allocation
    const size_t ring_buffer_size =
        (stream_info.ms_to_bytes(RING_BUFFER_DURATION_MS) / bytes_per_frame) *
        bytes_per_frame;
    std::shared_ptr<ring_buffer::RingBuffer> temp_ring_buffer =
        ring_buffer::RingBuffer::create(ring_buffer_size);
    if (temp_ring_buffer == nullptr) {
        this->status_momentary_error("ring_buffer", 15000);
        return false;
    }

    // Zero-copy source that reads directly from the ring buffer's internal
    // storage. Frame-aligned reads ensure multi-channel frames are never
    // split across the ring buffer's wrap boundary.
    this->audio_source_ = audio::RingBufferAudioSource::create(
        temp_ring_buffer, stream_info.ms_to_bytes(MAX_FILL_DURATION_MS),
        static_cast<uint8_t>(bytes_per_frame));
    if (this->audio_source_ == nullptr) {
        this->status_momentary_error("audio_source", 15000);
        return false;
    }
    this->ring_buffer_ = temp_ring_buffer;

    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock_ < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return false;
    }

    memset(&dest_addr_, 0, sizeof(dest_addr_));
    dest_addr_.sin_family = AF_INET;
    dest_addr_.sin_addr.s_addr = inet_addr(target_ip_str_.c_str());
    dest_addr_.sin_port = htons(port_);

    frame_counter_ = 0;

    this->status_clear_error();
    return true;
}

void VBANPublisherComponent::stop_() { this->audio_source_.reset(); }

} // namespace esphome::vban_publisher

#endif
