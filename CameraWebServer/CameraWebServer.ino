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

#define FRAME_WIDTH 64
#define FRAME_HEIGHT 64
#define MOTION_THRESHOLD 2

uint8_t *prevFrame;
uint8_t *currentFrame;
bool isFirstFrame = true;

volatile bool motionCheckFlag = false;

hw_timer_t * timer = NULL;

#include <esp32-hal-psram.h>

// AVI parameters
#define FRAMES_PER_SECOND 10
#define AVI_HEADER_SIZE 512
uint32_t frame_counter = 0;

bool isRecording = false;
unsigned long recordingStartTime = 0;
const unsigned long recordingDuration = 5000; // 5 seconds
File videoFile;
int fileCounter = 0;

uint32_t frame_offsets[50]; // Stores file positions of each frame
uint32_t frame_sizes[50];   // Stores sizes of each frame
uint16_t frame_index = 0; 

uint8_t *psramBuffer = NULL; // For SD card writes
size_t psramBufferSize = 0;

void IRAM_ATTR onTimer() {
  if (!isRecording) {
    motionCheckFlag = true;
  }
}

void startRecording() {
  char filename[32];
  snprintf(filename, sizeof(filename), "/motion_%d.mjpeg", fileCounter++);
  videoFile = SD_MMC.open(filename, FILE_WRITE);
  if (!videoFile) {
    Serial.println("Failed to create .mjpeg");
    return;
  }
  Serial.printf("Recording MJPEG to %s\n", filename);
  isRecording = true;
  recordingStartTime = millis();
}

void stopRecording() {
  if (!isRecording) return;
  videoFile.close();
  isRecording = false;
  isFirstFrame = true;
  Serial.println("MJPEG recording finished");
}

bool downscaleGrayscale(const camera_fb_t *fb, uint8_t *output) {
  if (fb->format != PIXFORMAT_JPEG) {
    Serial.println("Not a JPEG frame");
    return false;
  }

  uint32_t width = fb->width;
  uint32_t height = fb->height;

  // Allocate RGB888 buffer in PSRAM
  size_t rgb_len = width * height * 3;  // 3 bytes/pixel (RGB888)
  uint8_t *rgb_buf = (uint8_t*)heap_caps_malloc(rgb_len, MALLOC_CAP_SPIRAM);
  
  if (!rgb_buf) {
    Serial.println("Failed to allocate RGB buffer");
    return false;
  }
  
  bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb_buf);
  if (!converted || !rgb_buf) {
    Serial.println("JPEG to RGB conversion failed");
    return false;
  }

  // Calculate scaling ratios
  const float x_ratio = static_cast<float>(width) / FRAME_WIDTH;
  const float y_ratio = static_cast<float>(height) / FRAME_HEIGHT;

  // Process each target pixel
  for (int y = 0; y < FRAME_HEIGHT; y++) {
    for (int x = 0; x < FRAME_WIDTH; x++) {
      uint32_t gray_sum = 0;
      uint16_t samples = 0;

      // Sample 3x3 grid for anti-aliasing
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          // Calculate source coordinates with boundary checks
          int src_x = std::min(std::max(static_cast<int>((x + dx) * x_ratio), 0), static_cast<int>(width - 1));
          int src_y = std::min(std::max(static_cast<int>((y + dy) * y_ratio), 0), static_cast<int>(height - 1));

          // Get RGB values (3 bytes per pixel)
          uint8_t *pixel = &rgb_buf[(src_y * width + src_x) * 3];
          
          // Convert to grayscale using integer math
          uint8_t gray = static_cast<uint8_t>(
            (77 * pixel[0] +    // R component (0.299 * 255 ≈ 76.245)
           150 * pixel[1] +    // G component (0.587 * 255 ≈ 149.685)
            29 * pixel[2])     // B component (0.114 * 255 ≈ 29.07)
            >> 8);             // Divide by 256

          gray_sum += gray;
          samples++;
        }
      }

      // Store averaged result
      output[y * FRAME_WIDTH + x] = static_cast<uint8_t>(gray_sum / samples);
    }
  }

  free(rgb_buf);  // Free allocated RGB buffer
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

  //Serial.printf("Average diff: %.2f\n", avgDiff);
  return avgDiff > MOTION_THRESHOLD;
}

int getLastFileNumber() {
  int maxNumber = -1;
  File root = SD_MMC.open("/");
  if (!root) {
    Serial.println("Failed to open directory");
    return 0;
  }

  Serial.println("Scanning existing files:");
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String fileName = file.name();

      // Check both with and without leading slash
      if (fileName.startsWith("/motion_") || fileName.startsWith("motion_")) {
        if (fileName.endsWith(".mjpeg")) {
          // Remove leading slash if present
          if (fileName.startsWith("/")) {
            fileName = fileName.substring(1);
          }
          
          int start = fileName.indexOf('_') + 1;
          int end = fileName.indexOf('.');
          
          if (start > 0 && end > start) {
            String numberStr = fileName.substring(start, end);
            
            int fileNumber = numberStr.toInt();
            
            if (fileNumber > maxNumber) {
              maxNumber = fileNumber;
            }
          }
        }
      }
    }
    file = root.openNextFile();
  }
  
  Serial.print("Max file number found: ");
  Serial.println(maxNumber);
  return maxNumber + 1;
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
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
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
    s->set_framesize(s, FRAMESIZE_SVGA);
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

  delay(500); // Give time for camera to stabilize

  // Now initialize SD card after camera init
  SD_MMC.end();
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD Card Mount Failed");
  } else {
    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
      Serial.println("No SD card attached");
    } else {
      Serial.println("SD card initialized.");
      // Get next available file number
      fileCounter = getLastFileNumber();
      Serial.printf("Next file number: %d\n", fileCounter);
    }
  }

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
  delay(5000);

  // Timer setup for 500ms interval
  timer = timerBegin(1000000);                 // 1MHz → 1µs/tick
  timerAttachInterrupt(timer, &onTimer);       // Attach ISR function
  timerAlarm(timer, 500000, true, 0);          // Auto-reload every 500ms
}

void checkMotion() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return;
  }

  if (detectMotion(fb)) {
    Serial.println("Motion Detected!");
    if (!isRecording) {
      startRecording();
    }
  }

  esp_camera_fb_return(fb);
}

void loop() {
  if(motionCheckFlag) {
    motionCheckFlag = false;
    checkMotion();
  }

  if (isRecording) {
    static uint32_t last_frame = 0;
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      videoFile.write(fb->buf, fb->len);
      esp_camera_fb_return(fb);
      last_frame = millis();
    }
    if (millis() - recordingStartTime >= recordingDuration) {
      stopRecording();
    }
  }
}
