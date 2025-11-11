#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <driver/i2s.h>
#include <esp_spi_flash.h>
#include <Preferences.h>

Preferences preferences;

// Структура для хранения настроек Wi-Fi
struct WiFiSettings {
  char ssid[32];
  char password[64];
};

WiFiSettings wifiSettings;

// Настройки сервера
const char* serverURL = "http://###КАКОЙ ТО САЙТ###/upl8898.php";

// Настройки I2S для INMP441
#define I2S_WS 16
#define I2S_SD 15
#define I2S_SCK 37
#define I2S_PORT I2S_NUM_0

// Настройки аудио
#define SAMPLE_RATE 16000
#define BUFFER_SIZE 4096
#define RECORD_TIME 44 // секунд
#define SAMPLES_PER_BUFFER (BUFFER_SIZE / sizeof(int32_t))

// Порог активации записи
const int16_t ACTIVATION_THRESHOLD_AVG = 5000; // Средний уровень для активации
const int16_t DEACTIVATION_THRESHOLD_AVG = 1000; // Уровень для остановки (если тишина)
const int ACTIVATION_SAMPLES = 3; // Сколько раз подряд должен превысить порог

// Память для записи
static const size_t FLASH_BASE_ADDR = 0x2A0000 + SPI_FLASH_SEC_SIZE;
static size_t currentFlashAddr = 0;
static size_t totalSamplesRecorded = 0;
static const size_t MAX_SAMPLES = (SAMPLE_RATE * RECORD_TIME);

WebServer server(80);

// Буфер для аудиоданных
int32_t* audioBuffer = NULL;

bool isRecording = false;
bool isAnalyzing = true;
int activationCounter = 0;

// Оптимальное усиление - 128x дает хороший уровень без клиппинга
const float AUDIO_GAIN = 128.0f;

#pragma pack(push, 1)
typedef struct {
    char chunkID[4];        // "RIFF"
    uint32_t chunkSize;     // Размер файла - 8
    char format[4];         // "WAVE"
    char subchunk1ID[4];    // "fmt "
    uint32_t subchunk1Size; // 16 для PCM
    uint16_t audioFormat;   // 1 для PCM
    uint16_t numChannels;   // 1
    uint32_t sampleRate;    // 16000
    uint32_t byteRate;      // sampleRate * numChannels * bitsPerSample/8
    uint16_t blockAlign;    // numChannels * bitsPerSample/8
    uint16_t bitsPerSample; // 16
    char subchunk2ID[4];    // "data"
    uint32_t subchunk2Size; // Размер данных
} wav_header_t;
#pragma pack(pop)

// Флаги режима работы
bool isAPMode = false;
bool shouldStartAP = false;
unsigned long wifiConnectStartTime = 0;
const unsigned long WIFI_TIMEOUT = 20000; // 20 секунд таймаут

void uploadToServer();
void eraseFlashInternal();
void startAutoRecording();

// Загрузка настроек Wi-Fi из памяти
bool loadWiFiSettings() {
  preferences.begin("wifi-config", true);
  bool hasSettings = preferences.getBytesLength("settings") == sizeof(WiFiSettings);
  
  if (hasSettings) {
    preferences.getBytes("settings", &wifiSettings, sizeof(WiFiSettings));
    Serial.printf("Загружены настройки Wi-Fi: %s\n", wifiSettings.ssid);
  } else {
    Serial.println("Настроек Wi-Fi не найдено");
  }
  
  preferences.end();
  return hasSettings;
}

// Сохранение настроек Wi-Fi в память
void saveWiFiSettings(const char* ssid, const char* password) {
  preferences.begin("wifi-config", false);
  strncpy(wifiSettings.ssid, ssid, sizeof(wifiSettings.ssid) - 1);
  strncpy(wifiSettings.password, password, sizeof(wifiSettings.password) - 1);
  preferences.putBytes("settings", &wifiSettings, sizeof(WiFiSettings));
  preferences.end();
  Serial.printf("Сохранены настройки Wi-Fi: %s\n", ssid);
}

// Настройка веб-сервера для конфигурации
void setupConfigWebServer() {
  server.on("/", HTTP_GET, []() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Настройка Wi-Fi</title>
  <meta charset="UTF-8">
  <style>
    body { font-family: Arial; margin: 40px; }
    .container { max-width: 400px; margin: 0 auto; }
    input { width: 100%; padding: 10px; margin: 8px 0; }
    button { width: 100%; padding: 12px; background: #007bff; color: white; border: none; }
  </style>
</head>
<body>
  <div class="container">
    <h2>Настройка Wi-Fi</h2>
    <form action="/configure" method="POST">
      <input type="text" name="ssid" placeholder="SSID" required>
      <input type="password" name="password" placeholder="Пароль" required>
      <button type="submit">Подключиться</button>
    </form>
  </div>
</body>
</html>
)rawliteral";
    server.send(200, "text/html", html);
  });

  server.on("/configure", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    
    if (ssid.length() > 0) {
      saveWiFiSettings(ssid.c_str(), password.c_str());
      
      String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Настройки сохранены</title>
  <meta charset="UTF-8">
</head>
<body>
  <h2>Настройки сохранены!</h2>
  <p>Перезагружаем устройство для подключения к Wi-Fi...</p>
  <script>
    setTimeout(function() {
      window.location.href = "/";
    }, 3000);
  </script>
</body>
</html>
)rawliteral";
      server.send(200, "text/html", html);
      
      delay(3000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Ошибка: SSID не может быть пустым");
    }
  });
}

// Запуск точки доступа для настройки
void startAPMode() {
  Serial.println("Запуск точки доступа...");
  
  // Генерируем уникальное имя AP
  String apName = "ESP32-Audio-Recorder_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  
  WiFi.softAP(apName.c_str());
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  
  Serial.printf("Точка доступа запущена: %s\n", apName.c_str());
  Serial.printf("IP адрес: %s\n", apIP.toString().c_str());
  
  isAPMode = true;
  
  // Настраиваем веб-сервер для конфигурации
  setupConfigWebServer();
}

// Подключение к Wi-Fi как клиент
bool connectToWiFi() {
  Serial.printf("Подключение к Wi-Fi: %s\n", wifiSettings.ssid);
  
  WiFi.begin(wifiSettings.ssid, wifiSettings.password);
  wifiConnectStartTime = millis();
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
    attempts++;
    
    // Проверка таймаута (20 секунд)
    if (millis() - wifiConnectStartTime > WIFI_TIMEOUT) {
      Serial.println("\nТаймаут подключения к Wi-Fi");
      return false;
    }
    
    if (attempts > 30) { // Дополнительная проверка на 30 попыток
      Serial.println("\nПревышено количество попыток");
      return false;
    }
  }
  
  Serial.printf("\nПодключено к Wi-Fi! IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void createWavHeader(uint8_t* header, uint32_t dataSize) {
    wav_header_t wavHeader;
    
    memcpy(wavHeader.chunkID, "RIFF", 4);
    wavHeader.chunkSize = dataSize + sizeof(wav_header_t) - 8;
    memcpy(wavHeader.format, "WAVE", 4);
    memcpy(wavHeader.subchunk1ID, "fmt ", 4);
    wavHeader.subchunk1Size = 16;
    wavHeader.audioFormat = 1;
    wavHeader.numChannels = 1;
    wavHeader.sampleRate = 16000;
    wavHeader.byteRate = 16000 * 1 * 16 / 8;
    wavHeader.blockAlign = 1 * 16 / 8;
    wavHeader.bitsPerSample = 16;
    memcpy(wavHeader.subchunk2ID, "data", 4);
    wavHeader.subchunk2Size = dataSize;
    
    memcpy(header, &wavHeader, sizeof(wav_header_t));
}

void setupI2S() {
  Serial.println("Настройка I2S...");
  
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = true,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("Ошибка установки драйвера I2S: %d\n", err);
    return;
  }
  
  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("Ошибка настройки пинов I2S: %d\n", err);
    return;
  }
  
  Serial.println("I2S настроен успешно");
}

// Конвертация с оптимальным усилением
int16_t convertINMP441Sample(int32_t raw_sample) {
  int32_t sample_24bit = raw_sample >> 8;
  int64_t amplified = (int64_t)((float)sample_24bit * AUDIO_GAIN);
  int32_t sample_16bit = amplified / 256;
  
  if (sample_16bit > 32767) sample_16bit = 32767;
  if (sample_16bit < -32768) sample_16bit = -32768;
  
  return (int16_t)sample_16bit;
}

// Анализ уровня сигнала и проверка порога
bool checkActivationThreshold(int32_t* buffer, size_t samples) {
  int16_t maxConverted = 0;
  int16_t minConverted = 0;
  int64_t sumConverted = 0;
  
  for (size_t i = 0; i < samples; i++) {
    int16_t converted = convertINMP441Sample(buffer[i]);
    if (converted > maxConverted) maxConverted = converted;
    if (converted < minConverted) minConverted = converted;
    sumConverted += abs(converted);
  }
  
  int16_t avgConverted = sumConverted / samples;
  
  Serial.printf("Анализ: Max=%6d, Min=%6d, Avg=%6d", maxConverted, minConverted, avgConverted);
  
  // Проверка порога активации
  if (avgConverted > ACTIVATION_THRESHOLD_AVG) {
    activationCounter++;
    Serial.printf(" -> АКТИВАЦИЯ %d/%d\n", activationCounter, ACTIVATION_SAMPLES);
    
    if (activationCounter >= ACTIVATION_SAMPLES) {
      Serial.println("!!! ПОРОГ ПРЕВЫШЕН - ЗАПУСК ЗАПИСИ !!!");
      return true;
    }
  } else {
    activationCounter = 0;
    Serial.println(" -> тишина");
  }
  
  return false;
}

// Функция анализа звука (работает в фоне)
void soundAnalysisTask(void* parameter) {
  size_t bytesRead = 0;
  
  Serial.println("Задача анализа звука запущена");
  Serial.printf("Порог активации: Avg > %d (%d раз подряд)\n", ACTIVATION_THRESHOLD_AVG, ACTIVATION_SAMPLES);
  
  while (isAnalyzing) {
    if (!isRecording) {
      esp_err_t err = i2s_read(I2S_PORT, audioBuffer, BUFFER_SIZE, &bytesRead, portMAX_DELAY);
      
      if (err == ESP_OK && bytesRead > 0) {
        int samplesRead = bytesRead / sizeof(int32_t);
        
        // Проверяем порог активации
        if (checkActivationThreshold(audioBuffer, samplesRead)) {
          // Запускаем запись
          startAutoRecording();
        }
      }
    }
    
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  
  Serial.println("Анализ звука остановлен");
  vTaskDelete(NULL);
}

void processAfterRecording() {
  Serial.println("=== ОБРАБОТКА ПОСЛЕ ЗАПИСИ ===");
  
  // Отправляем на сервер
  if (currentFlashAddr > FLASH_BASE_ADDR) {
    Serial.println("Отправка файла на сервер...");
    uploadToServer();
  } else {
    Serial.println("Нет данных для отправки");
  }
  
  // Очищаем память
  eraseFlashInternal();
  
  // Перезапускаем анализ звука
  Serial.println("Перезапуск анализа звука...");
  isAnalyzing = true;
  activationCounter = 0;
  
  xTaskCreatePinnedToCore(
    soundAnalysisTask,
    "Sound Analysis",
    8192,
    NULL,
    1,
    NULL,
    1
  );
}


void recordAudioTask(void* parameter) {
  size_t bytesRead = 0;
  int readCount = 0;
  
  Serial.println("Задача записи запущена");
  
  while (isRecording && totalSamplesRecorded < MAX_SAMPLES) {
    esp_err_t err = i2s_read(I2S_PORT, audioBuffer, BUFFER_SIZE, &bytesRead, portMAX_DELAY);
    
    if (err != ESP_OK) {
      Serial.printf("Ошибка чтения I2S: %d\n", err);
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }
    
    if (bytesRead > 0) {
      int samplesRead = bytesRead / sizeof(int32_t);
      
      // Прогресс записи каждые 5 секунд
      if (readCount % 50 == 0) {
        int secondsRecorded = totalSamplesRecorded / SAMPLE_RATE;
        Serial.printf("Записано: %d секунд\n", secondsRecorded);
      }
      readCount++;
      
      // Конвертация в 16-бит
      int16_t* convertedBuffer = (int16_t*)malloc(samplesRead * sizeof(int16_t));
      
      for (int i = 0; i < samplesRead; i++) {
        convertedBuffer[i] = convertINMP441Sample(audioBuffer[i]);
      }
      
      // Запись во флеш-память
      size_t dataSize = samplesRead * sizeof(int16_t);
      
      if (currentFlashAddr + dataSize <= FLASH_BASE_ADDR + (MAX_SAMPLES * sizeof(int16_t))) {
        spi_flash_write(currentFlashAddr, (uint32_t*)convertedBuffer, dataSize);
        currentFlashAddr += dataSize;
        totalSamplesRecorded += samplesRead;
      } else {
        Serial.println("Достигнут предел памяти!");
        break;
      }
      
      free(convertedBuffer);
    }
    
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
  
  Serial.println("Запись завершена");
  isRecording = false;
  
  // После записи отправляем на сервер и перезапускаем анализ
  processAfterRecording();
  
  vTaskDelete(NULL);
}

void startAutoRecording() {
  if (isRecording) return;
  
  Serial.println("=== АВТОМАТИЧЕСКИЙ ЗАПУСК ЗАПИСИ ===");
  
  // Останавливаем анализ на время записи
  isAnalyzing = false;  
 
  currentFlashAddr = FLASH_BASE_ADDR;
  totalSamplesRecorded = 0;
  isRecording = true;
  
  // Запускаем запись
  xTaskCreatePinnedToCore(
    recordAudioTask,
    "Record Audio",
    16384,
    NULL,
    1,
    NULL,
    1
  );
}

void eraseFlashInternal() {
  Serial.println("Очистка флеш-памяти...");
  
  size_t totalDataSize = MAX_SAMPLES * sizeof(int16_t);
  size_t sectorsNeeded = (totalDataSize + SPI_FLASH_SEC_SIZE - 1) / SPI_FLASH_SEC_SIZE;
  
  for (int i = 0; i < sectorsNeeded + 1; i++) {
    spi_flash_erase_sector((FLASH_BASE_ADDR / SPI_FLASH_SEC_SIZE) + i);
  }
  
  currentFlashAddr = FLASH_BASE_ADDR;
  totalSamplesRecorded = 0;
  
  Serial.println("Флеш-память очищена");
}


void uploadToServer() {
    if (currentFlashAddr == FLASH_BASE_ADDR) {
        server.send(400, "text/plain", "Нет данных для отправки");
        return;
    }
    
    Serial.println("Начало загрузки на сервер...");
    
    HTTPClient http;
    http.begin(serverURL);
    http.addHeader("Content-Type", "audio/wav");
    http.addHeader("Connection", "keep-alive");
    
    size_t dataSize = currentFlashAddr - FLASH_BASE_ADDR;
    size_t totalSize = dataSize + 44;
    
    // Создаем WAV заголовок
    uint8_t wavHeader[44];
    createWavHeader(wavHeader, dataSize);
    
    // Отправляем заголовок отдельно
    http.addHeader("X-File-Size", String(totalSize));
    http.addHeader("X-File-Start", "true");
    
    int httpResponseCode = http.POST(wavHeader, 44);
    
    if (httpResponseCode <= 0) {
        Serial.printf("Ошибка отправки заголовка: %s\n", http.errorToString(httpResponseCode).c_str());
        server.send(500, "text/plain", "Ошибка при отправке заголовка");
        http.end();
        return;
    }
    
    String sessionId = http.getString();
    Serial.printf("Заголовок отправлен, сессия: %s\n", sessionId.c_str());
    
    // Отправляем аудиоданные частями
    const size_t CHUNK_SIZE = 8192; // 8KB chunks
    uint8_t* chunkBuffer = (uint8_t*)malloc(CHUNK_SIZE);
    size_t bytesSent = 0;
    size_t flashAddr = FLASH_BASE_ADDR;
    
    if (chunkBuffer == NULL) {
        server.send(500, "text/plain", "Ошибка выделения памяти для буфера");
        http.end();
        return;
    }
    
    bool uploadSuccess = true;
    
    while (bytesSent < dataSize && uploadSuccess) {
        size_t chunkSize = min(CHUNK_SIZE, dataSize - bytesSent);
        
        // Читаем чанк из флеш-памяти
        spi_flash_read(flashAddr, (uint32_t*)chunkBuffer, chunkSize);
        
        // Отправляем чанк
        http.addHeader("X-File-Session", sessionId);
        if (bytesSent + chunkSize >= dataSize) {
            http.addHeader("X-File-End", "true");
        }
        
        httpResponseCode = http.POST(chunkBuffer, chunkSize);
        
        if (httpResponseCode > 0) {
            flashAddr += chunkSize;
            bytesSent += chunkSize;
            
            // Прогресс каждые 100KB
            if (bytesSent % (100 * 1024) == 0) {
                int progress = (bytesSent * 100) / dataSize;
                Serial.printf("Отправлено: %d/%d байт (%d%%)\n", bytesSent, dataSize, progress);
            }
        } else {
            Serial.printf("Ошибка отправки чанка: %s\n", http.errorToString(httpResponseCode).c_str());
            uploadSuccess = false;
            break;
        }
        
        // Небольшая задержка для стабильности сети
        delay(5);
    }
    
    free(chunkBuffer);
    
    if (uploadSuccess && bytesSent == dataSize) {
        String response = http.getString();
        Serial.printf("Сервер ответил: %d - %s\n", httpResponseCode, response.c_str());
        server.send(200, "text/plain", "Данные успешно отправлены: " + response);
        Serial.println("Загрузка завершена успешно");
    } else {
        server.send(500, "text/plain", "Ошибка при отправке данных. Отправлено: " + String(bytesSent) + "/" + String(dataSize));
        Serial.println("Загрузка прервана");
    }
    
    http.end();
}

// Ручное управление через HTTP
void startRecording() {
  if (isRecording) {
    server.send(200, "text/plain", "Запись уже идет");
    return;
  }
  
  isAnalyzing = false;
  startAutoRecording();
  server.send(200, "text/plain", "Ручная запись начата");
}

void stopRecording() {
  isRecording = false;
  isAnalyzing = false;
  server.send(200, "text/plain", "Запись и анализ остановлены");
}

void startAnalysis() {
  if (isAnalyzing) {
    server.send(200, "text/plain", "Анализ уже идет");
    return;
  }
  
  isAnalyzing = true;
  activationCounter = 0;
  
  xTaskCreatePinnedToCore(
    soundAnalysisTask,
    "Sound Analysis",
    8192,
    NULL,
    1,
    NULL,
    1
  );
  
  server.send(200, "text/plain", "Анализ звука запущен");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== АВТОМАТИЧЕСКИЙ ДЕТЕКТОР ЗВУКА ===");
  Serial.printf("Порог активации: Avg > %d (%d раз подряд)\n", ACTIVATION_THRESHOLD_AVG, ACTIVATION_SAMPLES);
  
  bool hasWiFiSettings = loadWiFiSettings();

  if (hasWiFiSettings) {
    // Пытаемся подключиться к Wi-Fi
    if (!connectToWiFi()) {
      Serial.println("Не удалось подключиться к Wi-Fi, запуск точки доступа...");
      shouldStartAP = true;
    }
  } else {
    // Нет настроек - сразу запускаем точку доступа
    Serial.println("Настроек Wi-Fi нет, запуск точки доступа...");
    shouldStartAP = true;
  }
  
  // Запускаем точку доступа если нужно
  if (shouldStartAP) {
    startAPMode();
  }

  //WiFi.begin(ssid, password);
  //while (WiFi.status() != WL_CONNECTED) {
  //  delay(1000);
  //  Serial.println("Подключение к Wi-Fi...");
  //}
  Serial.println("Подключено к Wi-Fi");
  Serial.print("IP адрес: ");
  Serial.println(WiFi.localIP());

  setupI2S();
  
  audioBuffer = (int32_t*)malloc(BUFFER_SIZE);
  if (audioBuffer == NULL) {
    Serial.println("Ошибка выделения памяти для буфера!");
    return;
  }

  if (isAPMode) {
    // В режиме AP сервер уже настроен в setupConfigWebServer()
    Serial.println("Веб-сервер настроен для конфигурации");
  } else {
  // HTTP endpoints для ручного управления
    server.on("/start", HTTP_GET, startRecording);
    server.on("/stop", HTTP_GET, stopRecording);
    server.on("/analyze", HTTP_GET, startAnalysis);
    server.on("/wifi-reset", HTTP_GET, []() {
      preferences.begin("wifi-config", false);
      preferences.clear();
      preferences.end();
      server.send(200, "text/plain", "Настройки Wi-Fi сброшены. Перезагрузка...");
      delay(3000);
      ESP.restart();
    });
    Serial.println("Веб-сервер настроен для управления");
  }
  
  server.begin();
  Serial.println("HTTP сервер запущен");
  
  if (!isAPMode) {
    // Очистка флеш-памяти
    eraseFlashInternal();

    // Автоматический запуск анализа при старте
    xTaskCreatePinnedToCore(
      soundAnalysisTask,
      "Sound Analysis",
      8192,
      NULL,
      1,
      NULL,
      1
    );
  }

}

void loop() {
  server.handleClient();
}
