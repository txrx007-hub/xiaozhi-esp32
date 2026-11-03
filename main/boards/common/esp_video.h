#pragma once
#include "sdkconfig.h"

#include <lvgl.h>
#include <atomic>
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

    // WALL-E: give the picture's PSRAM back (~1 MB at 800x600) once its JPEG has been made or sent: nothing reads the pixels afterwards (a vision
    // question takes a new picture first). Width, height and format stay for LastFrameInfo(). The caller holds capture_mutex_.
    void ReleaseFrame();

    // WALL-E: called (with the capture lock held, so it must not take it) right after the sensor streams again following an idle stop.
    // Whatever lives in the sensor (focus, ...) can be put back from here, in a task of its own.
    virtual void OnStreamStarted() {}
    // WALL-E: called without the lock after a picture woke the stream, before it is taken: wait here until the sensor is ready (focus found).
    virtual void WaitSensorSettled() {}
    // WALL-E: how long frames are thrown away after the stream starts again (the sensor wakes up, its exposure settles).
    virtual int WarmupMs() const { return 1000; }

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
    const char* capture_error_ = "";

    // WALL-E: idle stop. The sensor streams 800x600 at 10 fps into PSRAM all the time; nobody looks at those frames between pictures, yet they cost
    // PSRAM bandwidth, CPU and heat (and the camera controller's memory). With idle_off_ the stream is stopped kIdleOffMs after the last use
    // and started again by the next picture (STREAMOFF deletes the camera controller and puts the sensor in software power-down).
    std::atomic<bool> stream_running_{false};   // the V4L2 stream is on (streaming_on_ only says the camera was set up)
    std::atomic<bool> idle_off_{false};
    std::atomic<int64_t> last_use_us_{0};
    int64_t stream_since_us_ = 0;
    uint64_t stream_on_total_us_ = 0;           // time the stream was on before the current run
    std::atomic<uint32_t> stream_starts_{0};
    std::atomic<uint32_t> stream_stops_{0};
    uint32_t last_warmup_frames_ = 0;
    void* idle_timer_ = nullptr;                // esp_timer_handle_t
    bool StreamStartLocked();                   // true when the stream runs and delivered a frame
    void StreamStopLocked();
    static void IdleTick(void* arg);
    bool CaptureJpegOnce(uint8_t quality, uint8_t** out, size_t* out_len, int frames, bool preview);
    bool CaptureImpl(int frames, bool preview);
    void Open(const esp_video_init_config_t& config);   // WALL-E: constructor / Restart()
    void Close();                                       // WALL-E: destructor / Restart()

public:
    EspVideo(const esp_video_init_config_t& config);
    ~EspVideo() override;

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    // WALL-E: quiet "is the sensor delivering frames at all" check - one DQBUF/QBUF, no copy, no
    // preview. True if a frame arrived within the DQBUF timeout (3 s).
    bool ProbeFrame();
    // WALL-E: shut the camera down completely and bring it up again in this same object (anything holding a pointer to it stays valid).
    // config must still be alive (the board keeps it static). True when the device streams again; frames still have to be checked.
    bool Restart(const esp_video_init_config_t& config, int pause_ms = 200);
    // WALL-E: one picture as a JPEG for the settings page's Camera button: no sound, no analysis, nothing sent anywhere. On success *out is the
    // JPEG (allocated here, the caller frees it with heap_caps_free) and *out_len its size.
    // frames = how many camera frames are taken before the one that is used (the buffers hold older ones: the first frame can be from before the camera moved);
    // preview = also show it on the robot's screen (only with the default 3 frames).
    bool CaptureJpeg(uint8_t quality, uint8_t** out, size_t* out_len, int frames = 3, bool preview = true);
    // WALL-E: why the last CaptureJpeg() failed, in words for the page and the log ("" after a success).
    const char* CaptureError() const { return capture_error_; }
    // WALL-E: the sensor's auto-exposure target (V4L2_CID_CAMERA_AE_LEVEL, -5..5, 0 = sensor default): higher = brighter pictures.
    bool SetAeLevel(int level);
    // WALL-E: write one raw sensor register (via esp_cam_sensor's ioctl pass-through).
    bool WriteSensorReg(uint16_t reg, uint8_t value);
    // WALL-E: read one raw sensor register (same pass-through).
    bool ReadSensorReg(uint16_t reg, uint8_t* value);
    // WALL-E: stop the sensor stream while no picture is wanted (see idle_off_ above); on = false starts it again at once. The next picture
    // after an idle stop takes about a second longer (the sensor starts and its exposure settles).
    void SetIdleOff(bool on);
    bool IdleOff() const { return idle_off_; }
    // WALL-E: start the stream now if it is stopped (blocks for the warm-up); true when it runs.
    bool WakeStream();
    bool StreamRunning() const { return stream_running_; }
    // "none" (no camera), "on" or "off" (stopped while idle); the counters are for /api/status and the log.
    const char* StreamState() const;
    uint32_t StreamStarts() const { return stream_starts_; }
    uint32_t StreamStops() const { return stream_stops_; }
    uint32_t StreamOnSeconds() const;
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
