#ifdef USE_ESP_IDF

#include "audio_reader.h"

#include "esphome/core/ring_buffer.h"
#include "esphome/core/hal.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

namespace esphome {
namespace nabu {

static const size_t READ_WRITE_TIMEOUT_MS = 20;

// The number of times the http read times out with no data before throwing an error
static const size_t ERROR_COUNT_NO_DATA_READ_TIMEOUT = 50;

AudioReader::AudioReader(esphome::RingBuffer *output_ring_buffer, size_t transfer_buffer_size) {
  this->output_ring_buffer_ = output_ring_buffer;
  this->transfer_buffer_size_ = transfer_buffer_size;
}

AudioReader::~AudioReader() {
  if (this->transfer_buffer_ != nullptr) {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    allocator.deallocate(this->transfer_buffer_, this->transfer_buffer_size_);
  }

  this->cleanup_connection_();
}

esp_err_t AudioReader::allocate_buffers_() {
  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  if (this->transfer_buffer_ == nullptr)
    this->transfer_buffer_ = allocator.allocate(this->transfer_buffer_size_);

  if (this->transfer_buffer_ == nullptr)
    return ESP_ERR_NO_MEM;

  return ESP_OK;
}

esp_err_t AudioReader::start(media_player::MediaFile *media_file, media_player::MediaFileType &file_type) {
  file_type = media_player::MediaFileType::NONE;

  esp_err_t err = this->allocate_buffers_();
  if (err != ESP_OK) {
    return err;
  }

  this->current_media_file_ = media_file;

  this->transfer_buffer_current_ = media_file->data;
  this->transfer_buffer_length_ = media_file->length;
  file_type = media_file->file_type;

  return ESP_OK;
}

esp_err_t AudioReader::start(const std::string &uri, media_player::MediaFileType &file_type) {
  file_type = media_player::MediaFileType::NONE;

  esp_err_t err = this->allocate_buffers_();
  if (err != ESP_OK) {
    return err;
  }

  this->cleanup_connection_();

  if (uri.empty()) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_http_client_config_t client_config = {};

  client_config.url = uri.c_str();
  client_config.cert_pem = nullptr;
  client_config.disable_auto_redirect = false;
  client_config.max_redirection_count = 10;
  client_config.buffer_size = 4 * 1024;
  client_config.keep_alive_enable = true;
  client_config.timeout_ms = 5000;  // Doesn't raise an error if exceeded in esp-idf v4.4, it just prevents the
                                    // http_client_read command from blocking for too long

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (uri.find("https:") != std::string::npos) {
    client_config.crt_bundle_attach = esp_crt_bundle_attach;
  }
#endif

  this->client_ = esp_http_client_init(&client_config);

  if (this->client_ == nullptr) {
    return ESP_FAIL;
  }

  if ((err = esp_http_client_open(this->client_, 0)) != ESP_OK) {
    this->cleanup_connection_();
    return err;
  }

  int content_length = esp_http_client_fetch_headers(this->client_);

  char url[500];
  err = esp_http_client_get_url(this->client_, url, 500);
  if (err != ESP_OK) {
    this->cleanup_connection_();
    return err;
  }

  std::string url_string = url;

  if (str_endswith(url_string, ".wav")) {
    file_type = media_player::MediaFileType::WAV;
  } else if (str_endswith(url_string, ".mp3")) {
    file_type = media_player::MediaFileType::MP3;
  } else if (str_endswith(url_string, ".flac")) {
    file_type = media_player::MediaFileType::FLAC;
  } else {
    file_type = media_player::MediaFileType::NONE;
    this->cleanup_connection_();
    return ESP_ERR_NOT_SUPPORTED;
  }

  err = esp_http_client_set_timeout_ms(this->client_, 10);
  if ( err != ESP_OK ){
    this->cleanup_connection_();
    return err;
  }
  
  this->transfer_buffer_current_ = this->transfer_buffer_;
  this->transfer_buffer_length_ = 0;

  return ESP_OK;
}

AudioReaderState AudioReader::read() {
  if (this->client_ != nullptr) {
    return this->http_read_();
  } else if (this->current_media_file_ != nullptr) {
    return this->file_read_();
  }

  return AudioReaderState::FAILED;
}

AudioReaderState AudioReader::file_read_() {
  if (this->transfer_buffer_length_ > 0) {
    size_t bytes_written = this->output_ring_buffer_->write_without_replacement(
        (void *) this->transfer_buffer_current_, this->transfer_buffer_length_, pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));
    this->transfer_buffer_length_ -= bytes_written;
    this->transfer_buffer_current_ += bytes_written;

    return AudioReaderState::READING;
  }
  return AudioReaderState::FINISHED;
}

AudioReaderState AudioReader::http_read_() {

  if (esp_http_client_is_complete_data_received(this->client_)) {
      this->cleanup_connection_();
      return AudioReaderState::FINISHED;
    }
  
  size_t space_available = this->output_ring_buffer_->free();
  size_t bytes_to_read = this->transfer_buffer_size_;
  if ( space_available < bytes_to_read ) {
    bytes_to_read = space_available;
  }
    int received_len = esp_http_client_read(
      this->client_, (char *) this->transfer_buffer_, bytes_to_read);

  if (received_len < 0) {
      this->cleanup_connection_();
      return AudioReaderState::FAILED;
      }
  
  if (received_len > 0) {
    size_t bytes_written = this->output_ring_buffer_->write_without_replacement(
        (void *) this->transfer_buffer_, received_len, pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));
    }

  if( received_len * 4 < this->transfer_buffer_size_ * 3 ){
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  return AudioReaderState::READING;
}

void AudioReader::cleanup_connection_() {
  if (this->client_ != nullptr) {
    esp_http_client_close(this->client_);
    esp_http_client_cleanup(this->client_);
    this->client_ = nullptr;
  }
}

}  // namespace nabu
}  // namespace esphome

#endif
