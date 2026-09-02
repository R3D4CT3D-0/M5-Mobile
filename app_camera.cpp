#include <M5Unified.h>
#include "p4_os.h"
#include <cstring>
#include <cerrno>
#include <cstdio>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_video_isp_ioctl.h"
#include <dirent.h>

static const char* TAG = "cam_app";

// enumerate /dev/videoN nodes and log their v4l2 capabilities, useful for
// figuring out which device node the camera actually landed on
void probeVideoDevices() {
    DIR* d = opendir("/dev");
    if (!d) {
        ESP_LOGE(TAG, "probeVideoDevices: can't open /dev");
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, "video", 5) == 0) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
            int fd = open(path, O_RDWR);
            if (fd < 0) {
                ESP_LOGW(TAG, "probe: %s open failed (errno=%d)", path, errno);
                continue;
            }
            struct v4l2_capability cap = {};
            if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                ESP_LOGI(TAG, "probe: %s -> driver=%s card=%s bus=%s",
                         path, cap.driver, cap.card, cap.bus_info);
            } else {
                ESP_LOGW(TAG, "probe: %s -> QUERYCAP failed (errno=%d)", path, errno);
            }
            close(fd);
        }
    }
    closedir(d);
}

static const int CAM_W = 1280;
static const int CAM_H = 720;
static const int CAM_BUF_COUNT = 2;

static const int TITLEBAR_H = 56;

static int      s_fd = -1;
static int      s_ispFd = -1;
static void*     s_mmapBuf[CAM_BUF_COUNT] = { nullptr, nullptr };
static size_t    s_mmapLen[CAM_BUF_COUNT] = { 0, 0 };

static volatile bool s_streaming   = false;
static bool      s_ccmReapplied  = false;
static uint32_t  s_ccmLastReapplyMs = 0;
static int       s_ccmReapplyCount  = 0;
static bool      s_openFailed = false;
static bool      s_exclusiveBus = false;

static char      s_statusMsg[64] = "Opening camera...";

static uint16_t* s_rotBuf = nullptr;

static void draw_titlebar();
static bool camera_open();
static void camera_close();

// the camera driver takes the internal i2c bus for itself while streaming, so
// give it back to the rest of the OS (touch, IMU, etc.) once we're done with it
static void restore_shared_bus() {

    if (!s_exclusiveBus) return;

    ((m5::M5Unified&)M5).In_I2C.begin();
    rotation_set_auto(true);
}

bool camera_uses_exclusive_i2c() {
    return s_exclusiveBus && s_streaming;
}

static const uint8_t IOEXP2_ADDR      = 0x43;
static const uint8_t IOEXP_REG_DIR    = 0x03;
static const uint8_t IOEXP_REG_OUT_HZ = 0x07;
static const uint8_t IOEXP_REG_OUT    = 0x05;
static const uint8_t CAM_RST_BIT      = 0x40;

// the camera's reset line is wired to a pin on the I2C GPIO expander rather
// than a directly addressable GPIO, so bring it out of reset by toggling that register
static void camera_release_reset() {
    auto& i2c = ((m5::M5Unified&)M5).In_I2C;

    uint8_t dir = i2c.readRegister8(IOEXP2_ADDR, IOEXP_REG_DIR, 400000);
    i2c.writeRegister8(IOEXP2_ADDR, IOEXP_REG_DIR, dir | CAM_RST_BIT, 400000);

    uint8_t hz = i2c.readRegister8(IOEXP2_ADDR, IOEXP_REG_OUT_HZ, 400000);
    i2c.writeRegister8(IOEXP2_ADDR, IOEXP_REG_OUT_HZ, hz & ~CAM_RST_BIT, 400000);

    uint8_t out = i2c.readRegister8(IOEXP2_ADDR, IOEXP_REG_OUT, 400000);
    i2c.writeRegister8(IOEXP2_ADDR, IOEXP_REG_OUT, out | CAM_RST_BIT, 400000);

    vTaskDelay(pdMS_TO_TICKS(10));
}

static bool s_xclkStarted = false;

// the sensor needs an external clock signal, which we generate with the LEDC
// PWM peripheral at 24MHz rather than a real dedicated clock output
static void camera_start_xclk() {
    if (s_xclkStarted) return;

    ledc_timer_config_t timer_conf = {};
    timer_conf.duty_resolution = LEDC_TIMER_1_BIT;
    timer_conf.freq_hz         = 24000000;
    timer_conf.speed_mode      = LEDC_LOW_SPEED_MODE;
    timer_conf.clk_cfg         = LEDC_AUTO_CLK;
    timer_conf.timer_num       = LEDC_TIMER_0;
    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cam xclk: ledc_timer_config failed, rc=0x%x", err);
    }

    ledc_channel_config_t ch_conf = {};
    ch_conf.gpio_num   = 36;
    ch_conf.speed_mode  = LEDC_LOW_SPEED_MODE;
    ch_conf.channel     = LEDC_CHANNEL_0;
    ch_conf.intr_type   = LEDC_INTR_DISABLE;
    ch_conf.timer_sel   = LEDC_TIMER_0;
    ch_conf.duty        = 1;
    ch_conf.hpoint      = 0;
    err = ledc_channel_config(&ch_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cam xclk: ledc_channel_config failed, rc=0x%x", err);
        return;
    }

    s_xclkStarted = true;
}

static bool camera_open() {
    rotation_set_auto(false);
    camera_start_xclk();
    camera_release_reset();

    ((m5::M5Unified&)M5).In_I2C.release();
    vTaskDelay(pdMS_TO_TICKS(50));
    s_exclusiveBus = true;

    esp_video_init_csi_config_t csi_config = {};
    csi_config.sccb_config.init_sccb = true;
    csi_config.sccb_config.i2c_config.port    = 0;
    csi_config.sccb_config.i2c_config.scl_pin = (gpio_num_t)32;
    csi_config.sccb_config.i2c_config.sda_pin = (gpio_num_t)31;
    csi_config.sccb_config.freq      = 400000;
    csi_config.reset_pin = GPIO_NUM_NC;
    csi_config.pwdn_pin  = GPIO_NUM_NC;

    esp_video_init_config_t video_config = {};
    video_config.csi = &csi_config;

    esp_err_t err = esp_video_init(&video_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: 0x%x", err);
        snprintf(s_statusMsg, sizeof(s_statusMsg), "esp_video_init failed (0x%x)", err);
        restore_shared_bus();
        return false;
    }
    ESP_LOGI(TAG, "esp_video_init OK — opening %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);

    {
        static bool probed = false;
        if (!probed) {
            probed = true;
            probeVideoDevices();
        }
    }

    s_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "open(%s) failed: errno=%d (%s)",
                 ESP_VIDEO_MIPI_CSI_DEVICE_NAME, errno, strerror(errno));
        snprintf(s_statusMsg, sizeof(s_statusMsg), "open failed: errno %d", errno);
        restore_shared_bus();
        return false;
    }
    ESP_LOGI(TAG, "open(%s) OK, fd=%d", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, s_fd);

    struct v4l2_capability cap = {};
    if (ioctl(s_fd, VIDIOC_QUERYCAP, &cap) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed");
        snprintf(s_statusMsg, sizeof(s_statusMsg), "No response from sensor");
        close(s_fd); s_fd = -1;
        restore_shared_bus();
        return false;
    }

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = CAM_W;
    fmt.fmt.pix.height      = CAM_H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(s_fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT failed");
        snprintf(s_statusMsg, sizeof(s_statusMsg), "Sensor rejected %dx%d RGB565", CAM_W, CAM_H);
        close(s_fd); s_fd = -1;
        restore_shared_bus();
        return false;
    }

    esp_log_level_set("ISP_CCM", ESP_LOG_NONE);
    esp_log_level_set("isp_video", ESP_LOG_NONE);
    esp_log_level_set("esp_video", ESP_LOG_NONE);
    esp_log_level_set("ISP", ESP_LOG_NONE);

    {
        // the ISP is exposed as a separate v4l2 node from the sensor itself;
        // walk /dev/videoN looking for the one whose driver/card name says "isp"
        for (int n = 0; n <= 4 && s_ispFd < 0; n++) {
            char path[20];
            snprintf(path, sizeof(path), "/dev/video%d", n);
            int fd = open(path, O_RDWR);
            if (fd < 0) continue;
            struct v4l2_capability cap = {};
            if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                bool isIsp = (strcasestr((char*)cap.driver, "isp") != nullptr)
                          || (strcasestr((char*)cap.card,   "isp") != nullptr);
                if (isIsp) {
                    ESP_LOGI(TAG, "ISP device: /dev/video%d (driver=%s card=%s)",
                             n, cap.driver, cap.card);
                    s_ispFd = fd;
                    break;
                }
            }
            close(fd);
        }
        if (s_ispFd < 0) {
            s_ispFd = open("/dev/video1", O_RDWR);
            if (s_ispFd >= 0)
                ESP_LOGW(TAG, "ISP node not identified by name — trying /dev/video1");
        }

        if (s_ispFd >= 0) {
            // auto white balance tends to hunt/drift on this sensor, so turn it off
            // and lock a neutral (1:1 gain, identity CCM) manual profile instead
            struct v4l2_control awbCtrl = {};
            awbCtrl.id    = V4L2_CID_AUTO_WHITE_BALANCE;
            awbCtrl.value = 0;
            if (ioctl(s_ispFd, VIDIOC_S_CTRL, &awbCtrl) == 0) {
                ESP_LOGI(TAG, "ISP AWB disabled OK");
            } else {
                ESP_LOGW(TAG, "ISP AWB disable failed (errno=%d) — will reapply CCM after STREAMON", errno);
            }

            esp_video_isp_wb_t wb = { .enable = true, .red_gain = 1.0f, .blue_gain = 1.0f };
            struct v4l2_ext_control wbCtrl = {};
            wbCtrl.id   = V4L2_CID_USER_ESP_ISP_WB;
            wbCtrl.p_u8 = (uint8_t*)&wb;
            wbCtrl.size = sizeof(wb);
            struct v4l2_ext_controls wbCtrls = {};
            wbCtrls.ctrl_class = V4L2_CID_USER_CLASS;
            wbCtrls.count      = 1;
            wbCtrls.controls   = &wbCtrl;
            if (ioctl(s_ispFd, VIDIOC_S_EXT_CTRLS, &wbCtrls) != 0) {
                ESP_LOGE(TAG, "manual WB lock failed (errno=%d)", errno);
            } else {
                ESP_LOGI(TAG, "manual WB lock OK (red=%.2f blue=%.2f)", wb.red_gain, wb.blue_gain);
            }

            struct {
                bool  enable;
                float matrix[3][3];
            } ccm = {
                .enable = true,
                .matrix = {
                    {1.0f, 0.0f, 0.0f},
                    {0.0f, 1.0f, 0.0f},
                    {0.0f, 0.0f, 1.0f},
                }
            };
            struct v4l2_ext_control ccmCtrl = {};
            ccmCtrl.id   = V4L2_CID_USER_ESP_ISP_CCM;
            ccmCtrl.p_u8 = (uint8_t*)&ccm;
            ccmCtrl.size = sizeof(ccm);
            struct v4l2_ext_controls ccmCtrls = {};
            ccmCtrls.ctrl_class = V4L2_CID_USER_CLASS;
            ccmCtrls.count      = 1;
            ccmCtrls.controls   = &ccmCtrl;
            if (ioctl(s_ispFd, VIDIOC_S_EXT_CTRLS, &ccmCtrls) != 0) {
                ESP_LOGE(TAG, "CCM pre-STREAMON write failed (errno=%d)", errno);
            } else {
                ESP_LOGI(TAG, "CCM pre-STREAMON write OK");
            }

            esp_video_isp_bf_t bf = {
                .enable = true,
                .level  = 20,
                .matrix = {
                    {1, 2, 1},
                    {2, 4, 2},
                    {1, 2, 1},
                },
            };
            struct v4l2_ext_control bfCtrl = {};
            bfCtrl.id   = V4L2_CID_USER_ESP_ISP_BF;
            bfCtrl.p_u8 = (uint8_t*)&bf;
            bfCtrl.size = sizeof(bf);
            struct v4l2_ext_controls bfCtrls = {};
            bfCtrls.ctrl_class = V4L2_CID_USER_CLASS;
            bfCtrls.count      = 1;
            bfCtrls.controls   = &bfCtrl;
            if (ioctl(s_ispFd, VIDIOC_S_EXT_CTRLS, &bfCtrls) != 0) {
                ESP_LOGE(TAG, "BF (denoise) write failed (errno=%d)", errno);
            } else {
                ESP_LOGI(TAG, "BF (denoise) enabled OK, level=%d", bf.level);
            }

            struct v4l2_control satCtrl = {};
            satCtrl.id    = V4L2_CID_SATURATION;
            satCtrl.value = 140;
            // saturation/contrast bumped above default — flat sensor output otherwise
            ioctl(s_ispFd, VIDIOC_S_CTRL, &satCtrl);

            struct v4l2_control contrastCtrl = {};
            contrastCtrl.id    = V4L2_CID_CONTRAST;
            contrastCtrl.value = 145;
            ioctl(s_ispFd, VIDIOC_S_CTRL, &contrastCtrl);

        } else {
            ESP_LOGW(TAG, "could not open ISP device -- CCM not applied");
        }
    }

    struct v4l2_requestbuffers req = {};
    req.count  = CAM_BUF_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0 || req.count < CAM_BUF_COUNT) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        snprintf(s_statusMsg, sizeof(s_statusMsg), "Buffer allocation failed");
        close(s_fd); s_fd = -1;
        restore_shared_bus();
        return false;
    }

    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%d] failed", i);
            camera_close();
            return false;
        }
        s_mmapBuf[i] = mmap(NULL, buf.length, PROT_READ, MAP_SHARED, s_fd, buf.m.offset);
        s_mmapLen[i] = buf.length;
        // sanity-check the mapped pointer actually lands in PSRAM/internal RAM —
        // seen bogus pointers from this driver on some SoC revisions
        if (s_mmapBuf[i] == MAP_FAILED ||
            !( (uintptr_t)s_mmapBuf[i] >= 0x48000000u && (uintptr_t)s_mmapBuf[i] <= 0x4bffffffu ) &&
            !( (uintptr_t)s_mmapBuf[i] >= 0x40800000u && (uintptr_t)s_mmapBuf[i] <= 0x40ffffffu )) {
            ESP_LOGE(TAG, "mmap[%d] returned invalid pointer %p (MAP_FAILED or outside valid P4 RAM)", i, s_mmapBuf[i]);
            s_mmapBuf[i] = nullptr;
            camera_close();
            return false;
        }
        if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "initial VIDIOC_QBUF[%d] failed", i);
            camera_close();
            return false;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        snprintf(s_statusMsg, sizeof(s_statusMsg), "Stream start failed");
        camera_close();
        return false;
    }

    s_streaming = true;
    return true;
}

static void camera_close() {
    if (s_fd >= 0) {
        if (s_streaming) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(s_fd, VIDIOC_STREAMOFF, &type);
            s_streaming = false;
        }
        for (int i = 0; i < CAM_BUF_COUNT; i++) {
            if (s_mmapBuf[i]) { munmap(s_mmapBuf[i], s_mmapLen[i]); s_mmapBuf[i] = nullptr; }
        }
        close(s_fd);
        s_fd = -1;
    }
    if (s_ispFd >= 0) {
        close(s_ispFd);
        s_ispFd = -1;
    }

    s_ccmReapplied     = false;
    s_ccmLastReapplyMs = 0;
    s_ccmReapplyCount  = 0;
    restore_shared_bus();
}

void camera_app_stop() {
    camera_close();
    s_openFailed = false;
}

void camera_app_draw() {
    auto& d = M5.Display;
    int W = d.width();

    d.fillScreen(COL_BLACK);
    draw_titlebar();
    navbar_draw("Camera");

    if (s_fd < 0 && !s_openFailed) {
        snprintf(s_statusMsg, sizeof(s_statusMsg), "Opening camera...");
        d.setFont(&fonts::Font4);
        d.setTextDatum(middle_center);
        d.setTextColor(COL_TEXT_DIM, COL_BLACK);
        d.drawString(s_statusMsg, W / 2, TITLEBAR_H + (d.height() - TITLEBAR_H - navbar_height()) / 2);

        if (!camera_open()) {
            s_openFailed = true;
        }
        os.dirty = true;
        return;
    }

    if (s_openFailed) {
        d.setFont(&fonts::Font4);
        d.setTextDatum(middle_center);
        d.setTextColor(COL_DANGER, COL_BLACK);
        d.drawString(s_statusMsg, W / 2, TITLEBAR_H + (d.height() - TITLEBAR_H - navbar_height()) / 2);
        return;
    }

}

static void draw_titlebar() {
    auto& d = M5.Display;
    int W = d.width();
    d.fillRect(0, 0, W, TITLEBAR_H, COL_STATUSBAR);
    d.drawFastHLine(0, TITLEBAR_H, W, 0x333355U);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_STATUSBAR);
    d.drawString("Camera", W / 2, TITLEBAR_H / 2);
}

void camera_app_update() {

    auto t = ((m5::M5Unified&)M5).Touch.getDetail();
    if (t.wasClicked()) {
        navbar_touch(t.x, t.y);
    }

    if (!s_streaming) return;

    struct v4l2_buffer buf = {};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s_fd, &fds);
    struct timeval tv = {0, 0};
    // non-blocking poll: skip this frame if the driver has nothing ready yet
    if (select(s_fd + 1, &fds, nullptr, nullptr, &tv) <= 0) return;

    if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) return;

    auto _mmapValid = [](void* p) -> bool {
        auto a = (uintptr_t)p;
        return p && ((a >= 0x48000000u && a <= 0x4bffffffu) ||
                     (a >= 0x40800000u && a <= 0x40ffffffu));
    };
    if (buf.index >= CAM_BUF_COUNT || !_mmapValid(s_mmapBuf[buf.index])) {
        ESP_LOGE(TAG, "DQBUF returned bad index %u or invalid mmap ptr %p — dropping frame",
                 buf.index, buf.index < CAM_BUF_COUNT ? s_mmapBuf[buf.index] : nullptr);
        ioctl(s_fd, VIDIOC_QBUF, &buf);
        return;
    }

    // the ISP silently reverts our manual CCM/WB/AWB settings for a few
    // seconds after streaming starts, so keep reapplying them for a while
    static const uint32_t CCM_REAPPLY_WINDOW_MS    = 3000;
    static const uint32_t CCM_REAPPLY_INTERVAL_MS  = 150;
    uint32_t nowMs = millis();
    bool dueForReapply = !s_ccmReapplied
        || (nowMs - s_ccmLastReapplyMs >= CCM_REAPPLY_INTERVAL_MS
            && nowMs < s_ccmLastReapplyMs + CCM_REAPPLY_WINDOW_MS + CCM_REAPPLY_INTERVAL_MS
            && s_ccmReapplyCount < (int)(CCM_REAPPLY_WINDOW_MS / CCM_REAPPLY_INTERVAL_MS) + 1);
    if (dueForReapply && s_ispFd >= 0) {
        if (!s_ccmReapplied) {
            s_ccmReapplied = true;
            s_ccmLastReapplyMs = nowMs;
        }
        s_ccmLastReapplyMs = nowMs;
        s_ccmReapplyCount++;
        struct {
            bool  enable;
            float matrix[3][3];
        } ccm = { .enable=true, .matrix={{1,0,0},{0,1,0},{0,0,1}} };
        struct v4l2_ext_control  ctrl  = {};
        struct v4l2_ext_controls ctrls = {};
        ctrl.id   = V4L2_CID_USER_ESP_ISP_CCM;
        ctrl.p_u8 = (uint8_t*)&ccm;
        ctrl.size = sizeof(ccm);
        ctrls.ctrl_class = V4L2_CID_USER_CLASS;
        ctrls.count      = 1;
        ctrls.controls   = &ctrl;
        ioctl(s_ispFd, VIDIOC_S_EXT_CTRLS, &ctrls);

        struct v4l2_control awbCtrl = {};
        awbCtrl.id    = V4L2_CID_AUTO_WHITE_BALANCE;
        awbCtrl.value = 0;
        ioctl(s_ispFd, VIDIOC_S_CTRL, &awbCtrl);

        esp_video_isp_bf_t bf = {
            .enable = true,
            .level  = 20,
            .matrix = {
                {1, 2, 1},
                {2, 4, 2},
                {1, 2, 1},
            },
        };
        struct v4l2_ext_control  bfCtrl  = {};
        struct v4l2_ext_controls bfCtrls = {};
        bfCtrl.id   = V4L2_CID_USER_ESP_ISP_BF;
        bfCtrl.p_u8 = (uint8_t*)&bf;
        bfCtrl.size = sizeof(bf);
        bfCtrls.ctrl_class = V4L2_CID_USER_CLASS;
        bfCtrls.count      = 1;
        bfCtrls.controls   = &bfCtrl;
        ioctl(s_ispFd, VIDIOC_S_EXT_CTRLS, &bfCtrls);

        esp_video_isp_wb_t wb = { .enable = true, .red_gain = 1.0f, .blue_gain = 1.0f };
        struct v4l2_ext_control  wbCtrl  = {};
        struct v4l2_ext_controls wbCtrls = {};
        wbCtrl.id   = V4L2_CID_USER_ESP_ISP_WB;
        wbCtrl.p_u8 = (uint8_t*)&wb;
        wbCtrl.size = sizeof(wb);
        wbCtrls.ctrl_class = V4L2_CID_USER_CLASS;
        wbCtrls.count      = 1;
        wbCtrls.controls   = &wbCtrl;
        ioctl(s_ispFd, VIDIOC_S_EXT_CTRLS, &wbCtrls);
    }

    // sensor is mounted rotated 90 degrees relative to the panel, so transpose
    // the frame into a scratch buffer before pushing it to the display
    static const bool ROTATE_CLOCKWISE = true;

    int outW = CAM_H;
    int outH = CAM_W;

    if (!s_rotBuf) {
        s_rotBuf = (uint16_t*)heap_caps_malloc((size_t)CAM_W * CAM_H * 2, MALLOC_CAP_SPIRAM);
        if (!s_rotBuf) {
            ESP_LOGE(TAG, "rotation buffer alloc failed (%d bytes)", CAM_W * CAM_H * 2);
        }
    }

    if (s_rotBuf) {
        const uint16_t* src = (const uint16_t*)s_mmapBuf[buf.index];
        if (!src) { ioctl(s_fd, VIDIOC_QBUF, &buf); return; }
        for (int j = 0; j < CAM_H; j++) {
            const uint16_t* srcRow = src + (size_t)j * CAM_W;
            if (ROTATE_CLOCKWISE) {

                int col = CAM_H - 1 - j;
                for (int i = 0; i < CAM_W; i++) {
                    s_rotBuf[(size_t)i * CAM_H + col] = srcRow[i];
                }
            } else {

                for (int i = 0; i < CAM_W; i++) {
                    s_rotBuf[(size_t)(CAM_W - 1 - i) * CAM_H + j] = srcRow[i];
                }
            }
        }

        int W = M5.Display.width();
        int H = M5.Display.height();
        int availH = H - TITLEBAR_H - navbar_height();
        if (availH < 1) availH = 1;
        int px = (W - outW) / 2;      if (px < 0) px = 0;
        int py = TITLEBAR_H;

        int drawH = outH;
        if (drawH > availH) drawH = availH;

        gfx_push_direct_noswap(px, py, outW, drawH, s_rotBuf);
    }

    ioctl(s_fd, VIDIOC_QBUF, &buf);
}
