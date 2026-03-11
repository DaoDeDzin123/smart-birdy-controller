#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_camera.h"

// ==== Пины подключения камеры OV2640 (стандартное для ESP32-CAM) ====
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    21
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      19
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM      5
#define Y2_GPIO_NUM      4
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

// ==== Пин PIR датчика ====
#define PIR_PIN 13

// ==== Настройки Wi-Fi ====
const char* ssid = "your_ssid";
const char* password = "your_password";

// ==== URL API для отправки снимков (ожидается POST с JPEG в теле) ====
const char* api_url = "http://your-api.com/upload";

// ==== Время блокировки после срабатывания (в секундах) ====
const int LOCK_TIME = 30;

// ==== Время ожидания стабилизации PIR при запуске (сек) ====
const int PIR_STABILIZE_TIME = 30;

// ==== Встроенный светодиод для индикации (GPIO2 на большинстве ESP32) ====
#define LED_PIN 2

// ==== Прототипы функций ====
bool take_send_photo();
void goToSleepWithPIR();
void goToSleepWithTimer(int seconds);
void blinkLED(int times, int delayMs);

void setup() {
  Serial.begin(115200);
  delay(1000); // Даём время на запуск монитора порта

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pinMode(PIR_PIN, INPUT_PULLDOWN); // Подтяжка к GND, чтобы избежать дребезга

  Serial.println("Фотоловушка запускается...");

  // Ждём стабилизации PIR (датчик выдаёт ложные срабатывания первые 30-60 секунд)
  Serial.printf("Ожидание стабилизации PIR %d секунд...\n", PIR_STABILIZE_TIME);
  delay(PIR_STABILIZE_TIME * 1000);

  // Инициализация камеры
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
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;     // 20 MHz
  config.pixel_format = PIXFORMAT_JPEG; // Съёмка в JPEG
  config.frame_size = FRAMESIZE_SVGA;    // 800x600 (можно изменить)
  config.jpeg_quality = 12;               // Качество JPEG (0-63, меньше = лучше)
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Ошибка инициализации камеры: 0x%x\n", err);
    blinkLED(10, 200); // Аварийная индикация
    delay(5000);
    ESP.restart();
  }

  // Определяем причину пробуждения
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    // Проснулись по PIR
    Serial.println("Пробуждение по PIR");
    blinkLED(2, 100);
    if (take_send_photo()) {
      Serial.println("Снимок отправлен");
    } else {
      Serial.println("Ошибка отправки снимка");
    }
    // Уходим в сон с блокировкой
    goToSleepWithTimer(LOCK_TIME);
  }
  else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    // Проснулись по таймеру после блокировки
    Serial.println("Пробуждение по таймеру (блокировка снята)");
    // Переходим в режим ожидания PIR
    goToSleepWithPIR();
  }
  else {
    // Первый запуск (холодный старт)
    Serial.println("Первый запуск, делаем контрольный снимок");
    blinkLED(3, 200);
    if (take_send_photo()) {
      Serial.println("Контрольный снимок отправлен");
    } else {
      Serial.println("Ошибка контрольного снимка");
    }
    // После контрольного снимка переходим в режим ожидания PIR
    goToSleepWithPIR();
  }
}

void loop() {
  // Никогда не выполняется, так как после setup сразу уходим в сон
}

// ===== Функция захвата и отправки фото =====
bool take_send_photo() {
  // Подключение к Wi-Fi
  Serial.print("Подключение к Wi-Fi");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) { // ~20 секунд таймаут
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Не удалось подключиться к Wi-Fi");
    return false;
  }
  Serial.println("WiFi подключен, IP: " + WiFi.localIP().toString());

  // Захват изображения с камеры
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Не удалось захватить кадр");
    return false;
  }
  Serial.printf("Снимок получен, размер: %u байт\n", fb->len);

  // Отправка на API
  HTTPClient http;
  http.begin(api_url);
  http.addHeader("Content-Type", "image/jpeg"); // Сервер должен ожидать JPEG
  http.setTimeout(10000); // Таймаут 10 секунд

  int httpResponseCode = http.POST(fb->buf, fb->len);

  bool success = false;
  if (httpResponseCode > 0) {
    Serial.printf("Ответ сервера: %d\n", httpResponseCode);
    if (httpResponseCode == 200 || httpResponseCode == 201) {
      success = true;
    }
  } else {
    Serial.printf("Ошибка отправки: %s\n", http.errorToString(httpResponseCode).c_str());
  }

  // Освобождение ресурсов
  esp_camera_fb_return(fb);
  http.end();

  // Отключаем Wi-Fi для экономии энергии
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  return success;
}

// ===== Уход в сон с пробуждением по PIR (с проверкой текущего состояния) =====
void goToSleepWithPIR() {
  Serial.println("Переход в сон с пробуждением по PIR");

  // Если PIR уже активен (HIGH), то сразу уйдём в сон с таймером на 10 секунд,
  // чтобы избежать ложного пробуждения.
  if (digitalRead(PIR_PIN) == HIGH) {
    Serial.println("PIR активен, уходим в сон с таймером на 10 секунд");
    goToSleepWithTimer(10);
    return;
  }

  // Настраиваем пробуждение по rising edge на пине PIR
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIR_PIN, 1); // 1 = HIGH
  esp_deep_sleep_start();
}

// ===== Уход в сон на указанное количество секунд (таймер) =====
void goToSleepWithTimer(int seconds) {
  Serial.printf("Переход в сон на %d секунд (блокировка)\n", seconds);
  esp_sleep_enable_timer_wakeup(seconds * 1000000LL); // микросекунды
  esp_deep_sleep_start();
}

// ===== Простая индикация встроенным светодиодом =====
void blinkLED(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(delayMs);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(delayMs);
  }
}
