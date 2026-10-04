#pragma once
#include "sdkconfig.h"

#include <lvgl.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"
#include "esp_video_init.h"
#include "jpg/image_to_jpeg.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

class EspVideo : public Camera {
protected:  // WALL-E: subclasses may read or replace the captured frame (PSRAM-allocated)
    struct FrameBuffer {
        uint8_t* data = nullptr;
        size_t len = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        v4l2_pix_fmt_t format = 0;
    } frame_;

private:
    v4l2_pix_fmt_t sensor_format_ = 0;
#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    uint16_t sensor_width_ = 0;
    uint16_t sensor_height_ = 0;
#endif  // CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    int video_fd_ = -1;
    bool streaming_on_ = false;
    struct MmapBuffer {
        void* start = nullptr;
        size_t length = 0;
    };
    std::vector<MmapBuffer> mmap_buffers_;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;
    std::mutex capture_mutex_;  // WALL-E: serializes Capture() and ProbeFrame() on video_fd_

public:
    EspVideo(const esp_video_init_config_t& config);
    ~EspVideo() override;

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    // WALL-E: quiet "is the sensor delivering frames at all" check - one DQBUF/QBUF, no copy, no
    // preview. True if a frame arrived within the DQBUF timeout (3 s).
    bool ProbeFrame();
    // WALL-E: write one raw sensor register (via esp_cam_sensor's ioctl pass-through).
    bool WriteSensorReg(uint16_t reg, uint8_t value);
    // WALL-E: a video device was opened, i.e. esp_video found and initialized a camera sensor.
    bool CameraPresent() const { return video_fd_ >= 0; }
    // WALL-E: the sensor model as the sensor itself reports it (chip ID read over SCCB): "OV3660",
    // "OV2640", "OV5640", or "unknown (PID 0x....)"; empty when no camera is present.
    std::string DetectSensorName();
    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::expected<std::string, std::string> Explain(const std::string& question) override;
};
