#include "esp_camera.h"
#include <WiFi.h>
#include "FS.h"
#include "SD_MMC.h"

//
// WARNING!!! PSRAM IC required for UXGA resolution and high JPEG quality
//            Ensure ESP32 Wrover Module or other board with PSRAM is selected
//            Partial images will be transmitted if image exceeds buffer size
//
//            You must select partition scheme from the board menu that has at least 3MB APP space.
//            Face Recognition is DISABLED for ESP32 and ESP32-S2, because it takes up from 15
//            seconds to process single frame. Face Detection is ENABLED if PSRAM is enabled as well

// ===================
// Select camera model
// ===================
//#define CAMERA_MODEL_WROVER_KIT // Has PSRAM
//#define CAMERA_MODEL_ESP_EYE  // Has PSRAM
//#define CAMERA_MODEL_ESP32S3_EYE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_PSRAM // Has PSRAM
//#define CAMERA_MODEL_M5STACK_V2_PSRAM // M5Camera version B Has PSRAM
//#define CAMERA_MODEL_M5STACK_WIDE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_ESP32CAM // No PSRAM
//#define CAMERA_MODEL_M5STACK_UNITCAM // No PSRAM
//#define CAMERA_MODEL_M5STACK_CAMS3_UNIT  // Has PSRAM
#define CAMERA_MODEL_AI_THINKER // Has PSRAM
//#define CAMERA_MODEL_TTGO_T_JOURNAL // No PSRAM
//#define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM
// ** Espressif Internal Boards **
//#define CAMERA_MODEL_ESP32_CAM_BOARD
//#define CAMERA_MODEL_ESP32S2_CAM_BOARD
//#define CAMERA_MODEL_ESP32S3_CAM_LCD
//#define CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3 // Has PSRAM
//#define CAMERA_MODEL_DFRobot_Romeo_ESP32S3 // Has PSRAM
#include "camera_pins.h"

#define FRAME_WIDTH 96
#define FRAME_HEIGHT 96
#define MOTION_THRESHOLD 25

uint8_t *prevFrame;
uint8_t *currentFrame;
bool isFirstFrame = true;

bool downscaleGrayscale(const camera_fb_t *fb, uint8_t *output) {
  if (fb->format != PIXFORMAT_RGB565) return false;

  float skipX = (float)fb->width / FRAME_WIDTH;
  float skipY = (float)fb->height / FRAME_HEIGHT;

  for (int y = 0; y < FRAME_HEIGHT; y++) {
    for (int x = 0; x < FRAME_WIDTH; x++) {
      float graySum = 0;
      int samples = 0;

      // Sample a 3x3 region for light smoothing (blur)
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          int srcX = min(max((int)((x + dx) * skipX), 0), (int)(fb->width - 1));
          int srcY = min(max((int)((y + dy) * skipY), 0), (int)(fb->height - 1));
          int index = (srcY * fb->width + srcX) * 2;

          if (index < 0 || index + 1 >= fb->len) continue;

          uint16_t pixel = fb->buf[index] | (fb->buf[index + 1] << 8);

          uint8_t r = (pixel >> 11) & 0x1F;
          uint8_t g = (pixel >> 5) & 0x3F;
          uint8_t b = pixel & 0x1F;

          r = (r * 255) / 31;
          g = (g * 255) / 63;
          b = (b * 255) / 31;

          uint8_t gray = (uint8_t)(0.299 * r + 0.587 * g + 0.114 * b);
          graySum += gray;
          samples++;
        }
      }

      output[y * FRAME_WIDTH + x] = (uint8_t)(graySum / samples);
    }
  }

  return true;
}

bool detectMotion(const camera_fb_t *fb) {

  if (!downscaleGrayscale(fb, currentFrame)) {
    Serial.println("Failed to process frame");
    return false;
  }

  if (isFirstFrame) {
    memcpy(prevFrame, currentFrame, FRAME_WIDTH * FRAME_HEIGHT);
    isFirstFrame = false;
    return false;
  }

  long diffSum = 0;
  for (int i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++) {
    diffSum += abs(currentFrame[i] - prevFrame[i]);
  }

  float avgDiff = (float)diffSum / (FRAME_WIDTH * FRAME_HEIGHT);
  memcpy(prevFrame, currentFrame, FRAME_WIDTH * FRAME_HEIGHT); // Update previous

  Serial.printf("Average diff: %.2f\n", avgDiff);
  return avgDiff > MOTION_THRESHOLD;
}

// ===========================
// Enter your WiFi credentials
// ===========================
const char *ssid = "";
const char *password = "";

void startCameraServer();
void setupLedFlash(int pin);

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  //config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

  prevFrame = (uint8_t*) heap_caps_malloc(FRAME_WIDTH * FRAME_HEIGHT, MALLOC_CAP_SPIRAM);
  currentFrame = (uint8_t*) heap_caps_malloc(FRAME_WIDTH * FRAME_HEIGHT, MALLOC_CAP_SPIRAM);

  if (!prevFrame || !currentFrame) {
    Serial.println("Failed to allocate PSRAM buffers");
    while(true);
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // drop down frame size for higher initial frame rate
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash(LED_GPIO_NUM);
#endif

  if (strlen(ssid) > 0) {
    // Try to connect to Wi-Fi
    WiFi.begin(ssid, password);
    WiFi.setSleep(false);

    Serial.print("WiFi connecting");
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
      delay(500);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected");
      startCameraServer();
      Serial.print("Camera Ready! Use 'http://");
      Serial.print(WiFi.localIP());
      Serial.println("' to connect");
      return;
    } else {
      Serial.println("\nWiFi connection failed!");
    }
  }

  // Start in Access Point mode if no credentials or failed connection
  const char *ap_ssid = "ESP32-CAM-AP";
  const char *ap_password = "12345678"; // Minimum 8 characters
  WiFi.softAP(ap_ssid, ap_password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("Access Point started. Connect to '");
  Serial.print(ap_ssid);
  Serial.print("' and go to http://");
  Serial.println(IP);

  startCameraServer();
}

void loop() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    delay(500);
    return;
  }

  Serial.printf("Got frame: %dx%d, format: %d, len: %d\n", fb->width, fb->height, fb->format, fb->len);

  if (fb->format != PIXFORMAT_RGB565) {
    Serial.println("Unexpected format, skipping");
  } else if (detectMotion(fb)) {
    Serial.println("Motion Detected!");
  } else {
    Serial.println("No Motion");
  }

  esp_camera_fb_return(fb);
  delay(500);
}
