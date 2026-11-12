#pragma once

#include "../lvgl_image.h"
#include "gifdec.h"
#include <lvgl.h>
#include <memory>
#include <functional>

/**
 * C++ implementation of LVGL GIF widget
 * Provides GIF animation functionality using gifdec library
 */
class LvglGif {
public:
    explicit LvglGif(const lv_img_dsc_t* img_dsc);
    virtual ~LvglGif();

    // LvglImage interface implementation
    virtual const lv_img_dsc_t* image_dsc() const;

    /**
     * Start/restart GIF animation
     */
    void Start();

    /**
     * Pause GIF animation
     */
    void Pause();

    /**
     * Resume GIF animation
     */
    void Resume();

    /**
     * Stop GIF animation and rewind to first frame
     */
    void Stop();

    /**
     * Check if GIF is currently playing
     */
    bool IsPlaying() const;

    /**
     * Check if GIF was loaded successfully
     */
    bool IsLoaded() const;

    /**
     * Get loop count
     */
    int32_t GetLoopCount() const;

    /**
     * Set loop count
     */
    void SetLoopCount(int32_t count);

    /**
     * Get loop delay in milliseconds (delay between loops)
     */
    uint32_t GetLoopDelay() const;

    /**
     * Set loop delay in milliseconds (delay between loops)
     * @param delay_ms Delay in milliseconds before starting next loop. 0 means no delay.
     */
    void SetLoopDelay(uint32_t delay_ms);

    /**
     * Get GIF dimensions
     */
    uint16_t width() const;
    uint16_t height() const;

    /**
     * Set frame update callback
     */
    void SetFrameCallback(std::function<void()> callback);

    // WALL-E: eye colour. The tint (0xrrggbb, -1 = off) is shared by all tintable GIFs. A tinted frame is the decoded frame with every pixel turned into the tint colour
    // at the pixel's own brightness (black stays black), written to a separate buffer, so GIFs with a solid black background do not turn the whole screen into the colour.
    static void SetTint(int32_t rgb);
    void SetTintable(bool tintable);   // true for the faces (not for the boot GIF); also applies the current tint to the frame on screen
    void RefreshTint();                // the tint changed: tint the current frame again (call image_dsc() afterwards and give it to the image)

private:
    bool tintable_ = false;
    bool tinted_ = false;              // image_dsc() hands out the tinted buffer
    uint8_t* tint_buf_ = nullptr;      // ARGB8888, PSRAM, only allocated while a tint is on
    lv_img_dsc_t tint_dsc_;
    void ApplyTint();
    // GIF decoder instance
    gd_GIF* gif_;
    
    // LVGL image descriptor
    lv_img_dsc_t img_dsc_;
    
    // Animation timer
    lv_timer_t* timer_;
    
    // Last frame update time
    uint32_t last_call_;
    
    // Animation state
    bool playing_;
    bool loaded_;
    
    // Loop delay configuration
    uint32_t loop_delay_ms_;      // Delay between loops in milliseconds
    bool loop_waiting_;           // Whether we're waiting for the next loop
    uint32_t loop_wait_start_;    // Timestamp when loop wait started
    
    // Frame update callback
    std::function<void()> frame_callback_;
    
    /**
     * Update to next frame
     */
    void NextFrame();
    
    /**
     * Cleanup resources
     */
    void Cleanup();
};
