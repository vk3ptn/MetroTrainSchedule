// Author: Phuong Nguyen (VK3PTN)
// Description: Display Metro Train schedule and weather information using ESP32 Dev Module
// Version:
//  1.0.0 Initial version for ESP32 Dev 2.8 Inch TFT LCD (Cheap Yellow Display Board) - 10/09/2026
//  1.0.1 Implemented http queries to get Metro Train data - 12/09/2026
//  1.0.2 Added current clock and weather data - 13/09/2026
//  1.0.3 Added display enhancements - 14/09/2026

#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

// ============================================================
// WIFI SETTINGS
// ============================================================

const char* WIFI_SSID     = "xxx";
const char* WIFI_PASSWORD = "xxx";

// ============================================================
// TIMEZONE
// Melbourne with automatic daylight saving
// ============================================================

const char* TIMEZONE =
  "AEST-10AEDT,M10.1.0,M4.1.0";

// ============================================================
// TRAIN SETTINGS
// ============================================================

// Altona Station
const char* STOP_ID = "G1005";
const char* PLATFORM = "P1";
const int DEPARTURE_LIMIT = 4;

// ============================================================
// WEATHER SETTINGS
// ============================================================

const char* weatherURL =
  "https://api.open-meteo.com/v1/forecast"
  "?latitude=-37.8676"
  "&longitude=144.8300"
  "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m"
  "&temperature_unit=celsius"
  "&wind_speed_unit=kmh"
  "&timezone=Australia%2FMelbourne";

// ============================================================
// TRAIN API ENDPOINT
// ============================================================

String jsonEndpoint =
  String("https://esp32-ptv-display-um8x.vercel.app/departures?") +
  "stop_id=" + STOP_ID +
  "&platform=" + PLATFORM +
  "&limit=" + String(DEPARTURE_LIMIT);

// ============================================================
// DISPLAY
// ============================================================

TFT_eSPI tft = TFT_eSPI();

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240
#define TFT_BACKLIGHT_PIN 21

#define COLOR_HEADER      TFT_NAVY
#define COLOR_BACKGROUND  TFT_BLACK
#define COLOR_TEXT        TFT_WHITE
#define COLOR_DEST        TFT_WHITE
#define COLOR_TIME        TFT_YELLOW
#define COLOR_ON_TIME     TFT_GREEN
#define COLOR_DELAY       TFT_RED
#define COLOR_SECONDARY   TFT_LIGHTGREY
#define COLOR_WEATHER     TFT_WHITE

// ============================================================
// CLOCK
// ============================================================
char currentTime[6];   // HH:MM

// ============================================================
// WEATHER CACHE
// ============================================================

float weatherTemperature = 0.0;
int weatherHumidity = 0;
int weatherCode = 0;
float weatherWindSpeed = 0.0;
// Wind direction in degrees
int weatherWindDirection = 0;
bool weatherAvailable = false;

// ============================================================
// DISPLAY SLEEP STATE
// ============================================================

bool displaySleeping = false;

// ============================================================
// UPDATE TIMERS
// ============================================================

unsigned long lastScheduleUpdate = 0;
unsigned long lastWeatherUpdate = 0;
const unsigned long SCHEDULE_INTERVAL = 30000;    // 30 seconds
const unsigned long WEATHER_INTERVAL = 600000;   // 10 minutes

// ============================================================
// FORWARD DECLARATIONS
// ============================================================

void updateClock();
void updateSchedule();
void updateWeather();
void drawWeatherFooter();
void drawWeatherIcon(int code, int x, int y);
void parseAndDisplayJSON(String payload);
String getWeatherDescription(int code);
String getWindDirection(int degrees);
void showError(String title, String message);
void checkDisplaySleep();
void turnDisplayOff();
void turnDisplayOn();


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  pinMode(
    TFT_BACKLIGHT_PIN,
    OUTPUT
  );

  digitalWrite(
    TFT_BACKLIGHT_PIN,
    HIGH
  );

  // ----------------------------------------------------------
  // INITIALISE DISPLAY
  // ----------------------------------------------------------

  tft.init();
  tft.setRotation(1);
  tft.invertDisplay(false);
  tft.fillScreen(
    COLOR_BACKGROUND
  );

  // ----------------------------------------------------------
  // STARTUP SCREEN
  // ----------------------------------------------------------

  tft.setTextSize(1);
  tft.setTextColor(
    TFT_WHITE,
    COLOR_BACKGROUND
  );

  tft.setCursor(
    10,
    10
  );

  tft.println(
    "Metro Train Schedule Display"
  );

  tft.setCursor(
    10,
    40
  );

  tft.println(
    "Connecting to Wi-Fi..."
  );


  // ----------------------------------------------------------
  // WI-FI
  // ----------------------------------------------------------

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("Wi-Fi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // ----------------------------------------------------------
  // CONFIGURE MELBOURNE LOCAL TIME
  // ----------------------------------------------------------

  configTzTime(
    TIMEZONE,
    "pool.ntp.org",
    "time.nist.gov"
  );

  Serial.println("Waiting for time synchronization...");
  struct tm timeinfo;

  if (getLocalTime(&timeinfo, 10000)) {
    Serial.println("Time synchronized.");
  } else {
    Serial.println("Failed to synchronize time.");
  }

  // ----------------------------------------------------------
  // CHECK WHETHER DISPLAY SHOULD BE SLEEPING
  // ----------------------------------------------------------

  checkDisplaySleep();

  if (displaySleeping) {
    return;
  }

  // ----------------------------------------------------------
  // INITIAL DISPLAY
  // ----------------------------------------------------------

  updateClock();
  updateSchedule();
  updateWeather();

  // ----------------------------------------------------------
  // START TIMERS
  // ----------------------------------------------------------

  lastScheduleUpdate = millis();
  lastWeatherUpdate = millis();
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop() {

  checkDisplaySleep();

  // ----------------------------------------------------------
  // If display is sleeping, don't update anything
  // ----------------------------------------------------------

  if (displaySleeping) {

    delay(1000);
    return;
  }

  updateClock();

  // ----------------------------------------------------------
  // Check Wi-Fi
  // ----------------------------------------------------------

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi disconnected. Reconnecting...");
    WiFi.reconnect();
    delay(5000);
    return;
  }

  unsigned long now = millis();

  // ----------------------------------------------------------
  // TRAIN SCHEDULE
  // ----------------------------------------------------------

  if (now - lastScheduleUpdate >=  SCHEDULE_INTERVAL) {
    lastScheduleUpdate = now;
    updateSchedule();
  }


  // ----------------------------------------------------------
  // WEATHER
  // ----------------------------------------------------------

  if (now - lastWeatherUpdate >= WEATHER_INTERVAL) {
    lastWeatherUpdate = now;
    updateWeather();
  }
}


// ============================================================
// DISPLAY SLEEP CHECK
// Display ON:  05:00 - 23:59
// Display OFF: 00:00 - 04:59
// ============================================================

void checkDisplaySleep() {

  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    return;
  }

  int hour = timeinfo.tm_hour;

  bool shouldSleep = (hour >= 0 && hour < 5);

  // ----------------------------------------------------------
  // Turn display OFF
  // ----------------------------------------------------------

  if (shouldSleep && !displaySleeping) {
    Serial.println("Midnight sleep period - turning display OFF.");
    turnDisplayOff();
    return;
  }

  // ----------------------------------------------------------
  // Turn display ON
  // ----------------------------------------------------------

  if (!shouldSleep && displaySleeping) {
    Serial.println("5:00am - waking display up.");
    turnDisplayOn();
  }
}


// ============================================================
// TURN DISPLAY OFF
// ============================================================

void turnDisplayOff() {

  displaySleeping = true;

  // ----------------------------------------------------------
  // Tell LCD controller to turn display off
  // ----------------------------------------------------------

  tft.writecommand(TFT_DISPOFF);

  // ----------------------------------------------------------
  // Turn TFT backlight off
  // ----------------------------------------------------------

  digitalWrite(TFT_BACKLIGHT_PIN, LOW);
  Serial.println("Display OFF.");
}


// ============================================================
// TURN DISPLAY ON
// ============================================================

void turnDisplayOn() {

  displaySleeping = false;

  // ----------------------------------------------------------
  // Turn TFT backlight on
  // ----------------------------------------------------------

  digitalWrite(TFT_BACKLIGHT_PIN, HIGH);

  // ----------------------------------------------------------
  // Tell LCD controller to turn display on
  // ----------------------------------------------------------

  tft.writecommand(TFT_DISPON);
  delay(100);

  // ----------------------------------------------------------
  // Completely redraw display
  // ----------------------------------------------------------

  tft.fillScreen(COLOR_BACKGROUND);

  updateClock();
  updateSchedule();
  updateWeather();

  // ----------------------------------------------------------
  // Reset timers
  // ----------------------------------------------------------

  lastScheduleUpdate = millis();
  lastWeatherUpdate = millis();

  Serial.println("Display ON.");
}


// ============================================================
// UPDATE TRAIN SCHEDULE
// ============================================================

void updateSchedule() {

  if (displaySleeping) {
    return;
  }

  HTTPClient http;

  Serial.println();
  Serial.println("--------------------------------");
  Serial.println("Requesting PTV data:");
  Serial.println(jsonEndpoint);

  // ----------------------------------------------------------
  // Start HTTP request
  // ----------------------------------------------------------

  http.begin(jsonEndpoint);
  int httpCode = http.GET();

  Serial.print("HTTP status: ");
  Serial.println(httpCode);

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.println("JSON response:");
    Serial.println(payload);
    parseAndDisplayJSON(payload);
  } else {
    Serial.print("HTTP error: ");
    Serial.println(http.errorToString(httpCode));

    showError("HTTP ERROR", String(httpCode));
  }

  http.end();
}


// ============================================================
// PARSE TRAIN JSON AND DISPLAY
// ============================================================

void parseAndDisplayJSON(String payload) {

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.print("JSON parsing failed: ");
    Serial.println(error.c_str());
    showError("JSON ERROR", error.c_str());
    return;
  }

  // ----------------------------------------------------------
  // GET STOP INFORMATION
  // ----------------------------------------------------------

  const char* stopName =
    doc["stop"]["name"] |
    "Unknown";

  const char* platform =
    doc["stop"]["platform"] |
    "?";

    tft.fillScreen(
    COLOR_BACKGROUND
  );

  // ==========================================================
  // HEADER
  // ==========================================================

  tft.fillRect(
    0,
    0,
    SCREEN_WIDTH,
    32,
    COLOR_HEADER
  );

  // ----------------------------------------------------------
  // STATION NAME
  // ----------------------------------------------------------

  tft.setTextColor(TFT_WHITE, COLOR_HEADER);
  tft.setTextSize(2);

  String strStopName = String(stopName);
  strStopName.toUpperCase();

  tft.drawString(
    strStopName,
    8,
    10
  );

  // ----------------------------------------------------------
  // PLATFORM
  // ----------------------------------------------------------

  tft.setTextSize(1);

  String platformText =
    "PLATFORM " +
    String(platform);


  tft.drawString(
    platformText,
    100,
    14
  );

  // ----------------------------------------------------------
  // CURRENT LOCAL TIME
  // ----------------------------------------------------------

  updateClock();
  tft.setTextSize(2);

  tft.drawString(
    currentTime,
    250,
    10
  );

  // ==========================================================
  // COLUMN HEADERS
  // ==========================================================

  tft.setTextColor(
    COLOR_SECONDARY,
    COLOR_BACKGROUND
  );

  tft.setTextSize(1);

  tft.drawString(
    "DESTINATION",
    8,
    42
  );

  tft.drawString(
    "TIME",
    200,
    42
  );

  tft.drawString(
    "MIN",
    280,
    42
  );

  // Separator

  tft.drawFastHLine(
    0,
    56,
    SCREEN_WIDTH,
    TFT_DARKGREY
  );

  // ==========================================================
  // GET DEPARTURES
  // ==========================================================

  JsonArray departures =
    doc["departures"].as<JsonArray>();

  int y = 65;
  int count = 0;

  for (
    JsonObject departure :
    departures
  ) {

    if (count >= DEPARTURE_LIMIT) {
      break;
    }

    const char* destination =
      departure["dest"] |
      "Unknown";

    const char* departureTime =
      departure["time"] |
      "--:--";

    int delay =
      departure["delay"] |
      0;

    int remaining =
      departure["remain"] |
      -1;

    // ROW SEPARATOR
 
    if (count > 0) {

      tft.drawFastHLine(
        5,
        y - 5,
        310,
        TFT_DARKGREY
      );
    }

    // ========================================================
    // DESTINATION
    // ========================================================

    tft.setTextColor(
      COLOR_DEST,
      COLOR_BACKGROUND
    );

    tft.setTextSize(2);

    String destText = String(destination);

    if (destText.length() > 16) {
      destText = destText.substring(0, 16);
    }

    tft.drawString(
      destText,
      8,
      y
    );

    // ========================================================
    // SCHEDULED TIME
    // ========================================================

    tft.setTextColor(
      COLOR_TIME,
      COLOR_BACKGROUND
    );


    tft.drawString(
      departureTime,
      200,
      y
    );

    // ========================================================
    // MINUTES REMAINING
    // ========================================================

    if (remaining >= 0) {
      tft.setTextColor(
        COLOR_ON_TIME,
        COLOR_BACKGROUND
      );

      tft.drawString(
        String(remaining),
        280,
        y
      );
    }

    // ========================================================
    // DELAY
    // ========================================================

    if (delay > 0) {
      tft.setTextColor(
        COLOR_DELAY,
        COLOR_BACKGROUND
      );

      tft.setTextSize(1);
      String delayText =
        "+" +
        String(delay / 60) +
        "m";

      tft.drawString(
        delayText,
        275,
        y + 20
      );
    }

    // --------------------------------------------------------
    // Next row
    // --------------------------------------------------------
    y += 40;
    count++;
  }

  // ==========================================================
  // NO DEPARTURES
  // ==========================================================

  if (count == 0) {
    tft.setTextSize(2);

    tft.setTextColor(
      TFT_RED,
      COLOR_BACKGROUND
    );

    tft.drawString(
      "NO DEPARTURES",
      65,
      100
    );
  }

  Serial.print("Displayed ");
  Serial.print(count);
  Serial.println(" departures.");

  drawWeatherFooter();
}


// ============================================================
// UPDATE CLOCK
// ============================================================

void updateClock() {

  struct tm timeinfo;


  if (
    !getLocalTime(
      &timeinfo
    )
  ) {

    return;
  }


  strftime(
    currentTime,
    sizeof(currentTime),
    "%H:%M",
    &timeinfo
  );
}


// ============================================================
// WEATHER DESCRIPTION
// ============================================================

String getWeatherDescription(int code) {

  if (code == 0)
    return "CLEAR";

  if (
    code == 1 ||
    code == 2
  )
    return "PARTLY CLOUDY";

  if (code == 3)
    return "CLOUDY";

  if (
    code == 45 ||
    code == 48
  )
    return "FOG";

  if (
    code >= 51 &&
    code <= 57
  )
    return "DRIZZLE";

  if (
    code >= 61 &&
    code <= 67
  )
    return "RAIN";

  if (
    code >= 71 &&
    code <= 77
  )
    return "SNOW";

  if (
    code >= 80 &&
    code <= 82
  )
    return "SHOWERS";

  if (code >= 95)
    return "STORM";

  return "UNKNOWN";
}


// ============================================================
// WIND DIRECTION
// ============================================================

String getWindDirection(int degrees) {

  const char* directions[] = {
    "N",
    "NE",
    "E",
    "SE",
    "S",
    "SW",
    "W",
    "NW"
  };

  int index = ((degrees + 22) % 360) / 45;

  return directions[index];
}

// ============================================================
// DRAW WEATHER ICON
// x/y = top-left position of icon area
// ============================================================

void drawWeatherIcon(int code, int x, int y) {

  // ----------------------------------------------------------
  // CLEAR / SUNNY
  // ----------------------------------------------------------

  if (code == 0) {

    // Sun centre
    tft.fillCircle(
      x + 10,
      y + 9,
      6,
      TFT_YELLOW
    );

    // Sun rays

    tft.drawLine(
      x + 10, y,
      x + 10, y + 3,
      TFT_YELLOW
    );

    tft.drawLine(
      x + 10, y + 15,
      x + 10, y + 19,
      TFT_YELLOW
    );

    tft.drawLine(
      x + 1, y + 9,
      x + 4, y + 9,
      TFT_YELLOW
    );

    tft.drawLine(
      x + 16, y + 9,
      x + 20, y + 9,
      TFT_YELLOW
    );

    tft.drawLine(
      x + 3, y + 2,
      x + 6, y + 5,
      TFT_YELLOW
    );

    tft.drawLine(
      x + 14, y + 14,
      x + 17, y + 17,
      TFT_YELLOW
    );

    tft.drawLine(
      x + 3, y + 17,
      x + 6, y + 14,
      TFT_YELLOW
    );

    tft.drawLine(
      x + 14, y + 5,
      x + 17, y + 2,
      TFT_YELLOW
    );

    return;
  }

  // ----------------------------------------------------------
  // PARTLY CLOUDY
  // ----------------------------------------------------------

  if (
    code == 1 ||
    code == 2
  ) {

    // Small sun

    tft.fillCircle(
      x + 8,
      y + 6,
      4,
      TFT_YELLOW
    );

    // Cloud

    tft.fillCircle(
      x + 11,
      y + 12,
      5,
      TFT_WHITE
    );

    tft.fillCircle(
      x + 17,
      y + 10,
      6,
      TFT_WHITE
    );

    tft.fillRect(
      x + 8,
      y + 12,
      15,
      6,
      TFT_WHITE
    );

    return;
  }

  // ----------------------------------------------------------
  // CLOUDY
  // ----------------------------------------------------------

  if (code == 3) {

    tft.fillCircle(
      x + 9,
      y + 11,
      6,
      TFT_LIGHTGREY
    );

    tft.fillCircle(
      x + 16,
      y + 9,
      7,
      TFT_WHITE
    );

    tft.fillRect(
      x + 7,
      y + 11,
      17,
      7,
      TFT_WHITE
    );

    return;
  }

  // ----------------------------------------------------------
  // FOG
  // ----------------------------------------------------------

  if (
    code == 45 ||
    code == 48
  ) {

    tft.drawFastHLine(
      x + 2,
      y + 6,
      20,
      TFT_LIGHTGREY
    );

    tft.drawFastHLine(
      x,
      y + 11,
      23,
      TFT_LIGHTGREY
    );

    tft.drawFastHLine(
      x + 3,
      y + 16,
      18,
      TFT_LIGHTGREY
    );

    return;
  }

  // ----------------------------------------------------------
  // DRIZZLE / RAIN / SHOWERS
  // ----------------------------------------------------------

  if (
    (code >= 51 && code <= 67) ||
    (code >= 80 && code <= 82)
  ) {

    // Cloud

    tft.fillCircle(
      x + 9,
      y + 7,
      5,
      TFT_WHITE
    );

    tft.fillCircle(
      x + 16,
      y + 6,
      6,
      TFT_WHITE
    );

    tft.fillRect(
      x + 7,
      y + 8,
      17,
      6,
      TFT_WHITE
    );

    // Rain drops

    tft.drawLine(
      x + 8, y + 17,
      x + 6, y + 21,
      TFT_CYAN
    );

    tft.drawLine(
      x + 14, y + 17,
      x + 12, y + 21,
      TFT_CYAN
    );

    tft.drawLine(
      x + 20, y + 17,
      x + 18, y + 21,
      TFT_CYAN
    );

    return;
  }

  // SNOW

  if (
    code >= 71 &&
    code <= 77
  ) {

    // Cloud

    tft.fillCircle(
      x + 9,
      y + 7,
      5,
      TFT_WHITE
    );

    tft.fillCircle(
      x + 16,
      y + 6,
      6,
      TFT_WHITE
    );

    tft.fillRect(
      x + 7,
      y + 8,
      17,
      6,
      TFT_WHITE
    );

    // Snow flakes

    tft.drawPixel(
      x + 7,
      y + 19,
      TFT_WHITE
    );

    tft.drawPixel(
      x + 13,
      y + 19,
      TFT_WHITE
    );

    tft.drawPixel(
      x + 19,
      y + 19,
      TFT_WHITE
    );

    return;
  }

  // ----------------------------------------------------------
  // STORM
  // ----------------------------------------------------------

  if (code >= 95) {
    // Cloud

    tft.fillCircle(
      x + 9,
      y + 7,
      5,
      TFT_WHITE
    );

    tft.fillCircle(
      x + 16,
      y + 6,
      6,
      TFT_WHITE
    );

    tft.fillRect(
      x + 7,
      y + 8,
      17,
      6,
      TFT_WHITE
    );

    // Lightning bolt

    tft.fillTriangle(
      x + 14, y + 13,
      x + 9,  y + 20,
      x + 14, y + 19,
      TFT_YELLOW
    );

    tft.fillTriangle(
      x + 14, y + 18,
      x + 19, y + 13,
      x + 14, y + 14,
      TFT_YELLOW
    );

    return;
  }

  // ----------------------------------------------------------
  // UNKNOWN
  // ----------------------------------------------------------

  tft.drawCircle(
    x + 11,
    y + 10,
    8,
    TFT_WHITE
  );

  tft.setTextSize(1);

  tft.setTextColor(
    TFT_WHITE,
    COLOR_HEADER
  );

  tft.drawString(
    "?",
    x + 9,
    y + 5
  );
}


// ============================================================
// UPDATE WEATHER FROM API
// ============================================================

void updateWeather() {

  if (displaySleeping) {
    return;
  }

  HTTPClient http;

  Serial.println();

  Serial.println(
    "--------------------------------"
  );

  Serial.println(
    "Requesting weather data:"
  );

  Serial.println(
    weatherURL
  );

  // ----------------------------------------------------------
  // HTTP REQUEST
  // ----------------------------------------------------------

  http.begin(
    weatherURL
  );

  int httpCode =
    http.GET();

  Serial.print(
    "Weather HTTP status: "
  );

  Serial.println(
    httpCode
  );

  // ==========================================================
  // SUCCESS
  // ==========================================================

  if (
    httpCode == HTTP_CODE_OK
  ) {

    String payload =
      http.getString();


    JsonDocument doc;

    DeserializationError error =
      deserializeJson(
        doc,
        payload
      );

    if (!error) {
      // ------------------------------------------------------
      // Temperature
      // ------------------------------------------------------

      weatherTemperature =
        doc["current"]["temperature_2m"] |
        0.0;

      // ------------------------------------------------------
      // Humidity
      // ------------------------------------------------------

      weatherHumidity =
        doc["current"]["relative_humidity_2m"] |
        0;

      // ------------------------------------------------------
      // Weather code
      // ------------------------------------------------------

      weatherCode =
        doc["current"]["weather_code"] |
        0;

      // ------------------------------------------------------
      // Wind speed
      // ------------------------------------------------------

      weatherWindSpeed =
        doc["current"]["wind_speed_10m"] |
        0.0;

      // ------------------------------------------------------
      // Wind direction
      // ------------------------------------------------------

      weatherWindDirection =
        doc["current"]["wind_direction_10m"] |
        0;

      // ------------------------------------------------------
      // Weather is valid
      // ------------------------------------------------------

      weatherAvailable = true;

      // ------------------------------------------------------
      // Serial debugging
      // ------------------------------------------------------

      Serial.println(
        "Weather updated."
      );

      Serial.print(
        "Temperature: "
      );

      Serial.println(
        weatherTemperature
      );

      Serial.print(
        "Humidity: "
      );

      Serial.println(
        weatherHumidity
      );

      Serial.print(
        "Wind speed: "
      );

      Serial.print(
        weatherWindSpeed
      );

      Serial.println(
        " km/h"
      );

      Serial.print(
        "Wind direction: "
      );

      Serial.print(
        weatherWindDirection
      );

      Serial.print(
        " degrees ("
      );

      Serial.print(
        getWindDirection(
          weatherWindDirection
        )
      );

      Serial.println(
        ")"
      );

      drawWeatherFooter();
    } else {
      Serial.print(
        "Weather JSON error: "
      );

      Serial.println(
        error.c_str()
      );
    }

  } else {
    Serial.print(
      "Weather HTTP error: "
    );

    Serial.println(
      http.errorToString(
        httpCode
      )
    );
  }

  http.end();
}


// ============================================================
// DRAW WEATHER FOOTER
// Example:
// [SUN] WEATHER  14C CLEAR  65% RH  10km SW
// ============================================================

void drawWeatherFooter() {

  if (
    !weatherAvailable ||
    displaySleeping
  ) {

    return;
  }

  // ----------------------------------------------------------
  // Clear footer
  // ----------------------------------------------------------

  tft.fillRect(
    0,
    220,
    SCREEN_WIDTH,
    20,
    COLOR_HEADER
  );

  // ----------------------------------------------------------
  // Draw weather icon
  // ----------------------------------------------------------

  drawWeatherIcon(
    weatherCode,
    3,
    220
  );

  // ----------------------------------------------------------
  // Weather text
  // ----------------------------------------------------------

  tft.setTextSize(1);

  tft.setTextColor(
    COLOR_WEATHER,
    COLOR_HEADER
  );

  String weatherText =
    "  WEATHER  " +
    String(weatherTemperature, 0) +
    "C  " +
    getWeatherDescription(
      weatherCode
    ) +
    "  " +
    String(weatherHumidity) +
    "% RH  " +
    String(weatherWindSpeed, 0) +
    "km " +
    getWindDirection(
      weatherWindDirection
    );


  // ----------------------------------------------------------
  // Draw weather text
  // ----------------------------------------------------------

  tft.drawString(
    weatherText,
    30,
    227
  );
}


// ============================================================
// DISPLAY ERROR
// ============================================================

void showError(
  String title,
  String message
) {

  if (displaySleeping) {
    return;
  }


  tft.fillScreen(
    COLOR_BACKGROUND
  );


  tft.setTextSize(1);


  tft.setTextColor(
    TFT_RED,
    COLOR_BACKGROUND
  );


  tft.drawString(
    title,
    10,
    20
  );


  tft.setTextColor(
    TFT_WHITE,
    COLOR_BACKGROUND
  );


  tft.drawString(
    message,
    10,
    55
  );
}
// EOF
