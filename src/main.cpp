/**
 * Moon Phase
 * ESP32S NodeMCU or XIAO ESP32-C3 + GC9A01 240x240 round (SPI, TFT_eSPI) + LVGL 8.3.11
 * Credit to nishad2m8 https://github.com/nishad2m8
 * What this does:
 *  - Connects to WiFi, syncs time via NTP
 *  - Updates the time/date labels every second from the system clock
 *  - Periodically fetches the daily moon phase from MET Norway's Sunrise API
 *    and updates the phase name label + moon image accordingly
 *
 * NOTE: TFT pin mapping lives in platformio.ini build_flags.
 * Configure Wi-Fi and MET Norway contact through the setup access point.
 */

#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TzDbLookup.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <atomic>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include "ui.h"
#include "geolocation_ca.h"

TFT_eSPI tft = TFT_eSPI();

const char *NTP_SERVER = "pool.ntp.org";

struct DeviceConfig {
    String ssid;
    String password;
    String timezone;
    String timezoneId;
    String contactEmail;
    bool autoDim = true;
    double latitude;
    double longitude;
};

struct ManualLocation {
    double latitude;
    double longitude;
    char timezoneId[64];
};

static DeviceConfig deviceConfig;
static WebServer configServer(80);
static DNSServer dnsServer;
static const char *CONFIG_NAMESPACE = "moonclock";
static const uint16_t DNS_PORT = 53;
static const char *GEOLOCATION_URL = "https://ipwho.is/?fields=success,message,latitude,longitude,city,timezone.id";

// ---- LVGL display buffer (partial, single buffer, internal RAM only) ----
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[240 * 40];
static lv_disp_drv_t disp_drv;
static bool displayDimmed = false;
static_assert(LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0, "Display dimming requires unswapped RGB565 colors.");

static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    if (displayDimmed) {
        for (uint32_t i = 0; i < w * h; i++) {
            color_p[i].full = (color_p[i].full >> 1) & 0x7BEF;
        }
    }

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}


static const lv_img_dsc_t *moon_frames[30] = {
    &ui_img_moon_moon_1_png,  &ui_img_moon_moon_2_png,  &ui_img_moon_moon_3_png,
    &ui_img_moon_moon_4_png,  &ui_img_moon_moon_5_png,  &ui_img_moon_moon_6_png,
    &ui_img_moon_moon_7_png,  &ui_img_moon_moon_8_png,  &ui_img_moon_moon_9_png,
    &ui_img_moon_moon_10_png, &ui_img_moon_moon_11_png, &ui_img_moon_moon_12_png,
    &ui_img_moon_moon_13_png, &ui_img_moon_moon_14_png, &ui_img_moon_moon_15_png,
    &ui_img_moon_moon_16_png, &ui_img_moon_moon_17_png, &ui_img_moon_moon_18_png,
    &ui_img_moon_moon_19_png, &ui_img_moon_moon_20_png, &ui_img_moon_moon_21_png,
    &ui_img_moon_moon_22_png, &ui_img_moon_moon_23_png, &ui_img_moon_moon_24_png,
    &ui_img_moon_moon_25_png, &ui_img_moon_moon_26_png, &ui_img_moon_moon_27_png,
    &ui_img_moon_moon_28_png, &ui_img_moon_moon_29_png, &ui_img_moon_moon_30_png,
};

// ---- Update timers ----
const uint32_t CLOCK_UPDATE_MS = 1000;                 // refresh time/date label every second
const uint32_t MOON_UPDATE_MS = 60UL * 60UL * 1000UL;  // refetch hourly after the first successful reading
const uint32_t MOON_RETRY_MS = 15UL * 1000UL;          // retry this often until the FIRST fetch succeeds
const uint64_t GEOLOCATION_RETRY_MS = 5ULL * 60ULL * 1000ULL;

static uint32_t lastClockMillis = 0;
static uint32_t lastMoonMillis = 0;
static uint32_t lastAutoDimCheckMillis = 0;
static bool moonDataValid = false;
static bool locationValid = false;
static bool geolocationPending = true;
static uint64_t nextGeolocationAttemptMs = 0;
static bool manualLocation = false;
static std::atomic<bool> ipAddressPending(false);
static uint32_t ipShownAt = 0;
static bool showingIp = false;
static String settingsToken;

// Extra label added under the clock face for local moonrise/moonset.
// (Not part of the SquareLine Studio export, so it's safe from re-exports.)
static lv_obj_t *ui_moon_times = NULL;
static lv_obj_t *ui_time_period = NULL;
static lv_obj_t *ui_ip_overlay = NULL;
static lv_obj_t *ui_ip_label = NULL;
static const lv_point_t moonriseArrowPoints[] = {{0, 4}, {4, 0}, {8, 4}, {4, 0}, {4, 11}};
static const lv_point_t moonsetArrowPoints[] = {{4, 0}, {4, 11}, {0, 7}, {4, 11}, {8, 7}};

// ---- Forward declarations ----
bool loadDeviceConfig();
bool saveDeviceConfig(const DeviceConfig &config);
bool isValidConfig(const DeviceConfig &config);
bool isValidContactEmail(const String &email);
bool connectToWiFi();
void startProvisioningPortal();
void handleConfigurationSave();
String configurationPage(const String &message, bool autoDim);
String htmlEscape(const String &value);
String autoDimCheckbox(bool enabled);
bool saveAutoDim(bool enabled);
void updateAutoDim();
void pollAutoDim();
bool loadLocationMode();
bool loadManualLocation();
bool saveManualLocation(const ManualLocation &location);
bool useAutomaticLocation();
void startSettingsServer();
void handleSettingsSave();
void handleAutoDimSave();
String settingsPage(const String &message, const String &mode, const String &latitude,
                    const String &longitude, const String &timezoneId, bool autoDim);
String currentSettingsPage(const String &message, bool autoDim);
void showIpAddress();
void updateIpDisplay();
bool loadCachedLocation();
bool saveCachedLocation();
bool fetchLocation(DeviceConfig &result, uint64_t &retryMs);
bool applyLocation(const DeviceConfig &config);
void updateGeolocation();
bool waitForTimeSync(uint32_t timeoutMs);
void updateClockLabels();
void updateMoonData();
void setMoonPhaseLabel(const char *text);
String getMoonPhase(double age);
int getMoonImageIndex(double age);
void setMoonImage(int index);
void playMoonIntroAnimation();

void setup()
{
    Serial.begin(115200);
    tft.begin();
    tft.setRotation(0); // Try 0 if image is upside down.
    tft.fillScreen(TFT_BLACK);
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, 240 * 40);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 240;
    disp_drv.ver_res = 240;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    ui_init();
    lv_label_set_text(ui_time, "--:--:--");
    lv_label_set_text(ui_date, "-- --- --");

    ui_time_period = lv_label_create(ui_Screen1);
    lv_obj_set_width(ui_time_period, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_time_period, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(ui_time_period, lv_color_hex(0xFFFF00), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_time_period, &ui_font_datefont, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_time_period, "");
    lv_obj_align_to(ui_time_period, ui_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -2);

    ui_moon_times = lv_label_create(ui_Screen1);
    lv_obj_set_width(ui_moon_times, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_moon_times, LV_SIZE_CONTENT);
    // To the right of the time, same vertical neighborhood, moonrise
    // stacked above moonset.
    lv_obj_align(ui_moon_times, LV_ALIGN_CENTER, 68, 38);
    lv_obj_set_style_text_color(ui_moon_times, lv_color_hex(0xDBDBDB), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_moon_times, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_moon_times, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_moon_times, &ui_font_datefont, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_moon_times, "   --:--\n   --:--");

    lv_obj_t *moonriseArrow = lv_line_create(ui_moon_times);
    lv_line_set_points(moonriseArrow, moonriseArrowPoints, 5);
    lv_obj_set_style_line_color(moonriseArrow, lv_color_hex(0xDBDBDB), LV_PART_MAIN);
    lv_obj_set_style_line_width(moonriseArrow, 2, LV_PART_MAIN);
    lv_obj_align(moonriseArrow, LV_ALIGN_TOP_LEFT, 1, 3);

    lv_obj_t *moonsetArrow = lv_line_create(ui_moon_times);
    lv_line_set_points(moonsetArrow, moonsetArrowPoints, 5);
    lv_obj_set_style_line_color(moonsetArrow, lv_color_hex(0xDBDBDB), LV_PART_MAIN);
    lv_obj_set_style_line_width(moonsetArrow, 2, LV_PART_MAIN);
    lv_obj_align(moonsetArrow, LV_ALIGN_TOP_LEFT, 1,
                 3 + lv_font_get_line_height(&ui_font_datefont) +
                 lv_obj_get_style_text_line_space(ui_moon_times, LV_PART_MAIN));

    ui_ip_overlay = lv_obj_create(ui_Screen1);
    lv_obj_set_size(ui_ip_overlay, 240, 240);
    lv_obj_center(ui_ip_overlay);
    lv_obj_set_style_bg_color(ui_ip_overlay, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ip_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ip_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ip_overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ui_ip_overlay, LV_OBJ_FLAG_SCROLLABLE);
    ui_ip_label = lv_label_create(ui_ip_overlay);
    lv_obj_set_style_text_color(ui_ip_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_ip_label, &ui_font_datefont, LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_ip_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(ui_ip_label);
    lv_obj_add_flag(ui_ip_overlay, LV_OBJ_FLAG_HIDDEN);

    bool hasDeviceConfig = loadDeviceConfig();

    lv_task_handler(); 
    playMoonIntroAnimation();

    if (!hasDeviceConfig) {
        startProvisioningPortal();
    }
    WiFi.onEvent([](WiFiEvent_t) {
        ipAddressPending.store(true);
    }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    if (!connectToWiFi()) {
        startProvisioningPortal();
    }

    configTime(0, 0, NTP_SERVER);
    if (!loadLocationMode()) {
        geolocationPending = false;
    } else if (manualLocation) {
        geolocationPending = false;
        locationValid = loadManualLocation();
    } else {
        locationValid = loadCachedLocation();
    }
    if (locationValid) {
        Serial.println(manualLocation ? "Using manual location." : "Using last successful IP-based location until refreshed.");
        locationValid = applyLocation(deviceConfig);
    }
    if (!locationValid) {
        setMoonPhaseLabel("Location error");
    }
    startSettingsServer();
    ipAddressPending.store(false);
    showIpAddress();
    while (showingIp) {
        configServer.handleClient();
        lv_task_handler();
        pollAutoDim();
        updateIpDisplay();
        delay(5);
    }

    waitForTimeSync(15000);
    updateAutoDim();
    updateGeolocation();
    if (locationValid && (manualLocation || geolocationPending)) updateMoonData();
    lastClockMillis = millis();
    lastMoonMillis = millis();
    Serial.println("Setup complete.");
}

void loop()
{
    lv_task_handler();
    pollAutoDim();
    if (ipAddressPending.exchange(false) && WiFi.status() == WL_CONNECTED) {
        configServer.begin();
        showIpAddress();
    }
    if (WiFi.status() == WL_CONNECTED) configServer.handleClient();
    updateIpDisplay();
    if (showingIp) {
        delay(5);
        return;
    }
    updateGeolocation();
    uint32_t now = millis();
    if (locationValid && now - lastClockMillis >= CLOCK_UPDATE_MS) {
        lastClockMillis = now;
        updateClockLabels();
    }

    if (locationValid && now - lastMoonMillis >= (moonDataValid ? MOON_UPDATE_MS : MOON_RETRY_MS)) {
        lastMoonMillis = now;
        updateMoonData();
    }

    delay(5);
}

bool loadDeviceConfig()
{
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, true)) return false;

    bool configured = preferences.getBool("configured", false);
    if (configured) {
        deviceConfig.ssid = preferences.getString("ssid", "");
        deviceConfig.password = preferences.getString("password", "");
        deviceConfig.contactEmail = preferences.getString("contactEmail", "");
    }
    deviceConfig.autoDim = preferences.getBool("autoDim", true);
    preferences.end();
    return configured && isValidConfig(deviceConfig);
}

bool saveDeviceConfig(const DeviceConfig &config)
{
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, false)) return false;

    bool saved = preferences.putString("contactEmail", config.contactEmail) > 0 &&
                 preferences.putString("ssid", config.ssid) > 0;
    if (saved) {
        preferences.putString("password", config.password);
        saved = preferences.isKey("password") &&
                preferences.getString("password", "") == config.password &&
                preferences.putBool("autoDim", config.autoDim) > 0 &&
                preferences.putBool("configured", true) > 0;
    }
    preferences.end();
    return saved;
}

bool isValidConfig(const DeviceConfig &config)
{
    const size_t passwordLength = config.password.length();
    return config.ssid.length() > 0 && config.ssid.length() <= 32 &&
           (passwordLength == 0 || (passwordLength >= 8 && passwordLength <= 63)) &&
           isValidContactEmail(config.contactEmail);
}

bool isValidContactEmail(const String &email)
{
    if (email.length() == 0 || email.length() > 254) return false;
    int at = email.indexOf('@');
    if (at <= 0 || at > 64 || at == (int)email.length() - 1 || email.indexOf('@', at + 1) != -1) return false;

    for (int i = 0; i < at; i++) {
        char c = email[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || (c != '\0' && strchr(".!#$%&'*+/=?^_`{|}~-", c)))) return false;
    }

    size_t labelLength = 0;
    for (size_t i = at + 1; i < email.length(); i++) {
        char c = email[i];
        bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9');
        if (c == '.') {
            if (labelLength == 0 || email[i - 1] == '-') return false;
            labelLength = 0;
        } else {
            if ((!alphanumeric && c != '-') || (labelLength == 0 && c == '-')) return false;
            if (++labelLength > 63) return false;
        }
    }
    return labelLength > 0 && email[email.length() - 1] != '-';
}

String htmlEscape(const String &value)
{
    String escaped;
    escaped.reserve(value.length());
    for (size_t i = 0; i < value.length(); i++) {
        switch (value[i]) {
        case '&': escaped += "&amp;"; break;
        case '"': escaped += "&quot;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        default: escaped += value[i]; break;
        }
    }
    return escaped;
}

String autoDimCheckbox(bool enabled)
{
    return String("<label for=\"autoDim\"><input id=\"autoDim\" name=\"autoDim\" type=\"checkbox\" value=\"1\"") +
           (enabled ? " checked" : "") + ">Auto-dim display from 10 p.m. to 8 a.m. local time</label>";
}

bool saveAutoDim(bool enabled)
{
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, false)) return false;
    bool saved = preferences.putBool("autoDim", enabled) > 0;
    preferences.end();
    return saved;
}

void updateAutoDim()
{
    bool shouldDim = false;
    if (deviceConfig.autoDim && locationValid) {
        time_t now;
        struct tm localTime;
        time(&now);
        if (localtime_r(&now, &localTime) && localTime.tm_year >= (2023 - 1900)) {
            shouldDim = localTime.tm_hour >= 22 || localTime.tm_hour < 8;
        }
    }
    if (shouldDim != displayDimmed) {
        displayDimmed = shouldDim;
        lv_obj_invalidate(ui_Screen1);
        lv_refr_now(NULL);
    }
}

void pollAutoDim()
{
    uint32_t now = millis();
    if (now - lastAutoDimCheckMillis >= CLOCK_UPDATE_MS) {
        lastAutoDimCheckMillis = now;
        updateAutoDim();
    }
}

String configurationPage(const String &message, bool autoDim)
{
    String page = "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                  "<title>Moon Phase Clock Setup</title><style>body{font:16px sans-serif;max-width:32rem;margin:2rem auto;padding:0 1rem;background:#101820;color:#f2f4f3}"
                  "label{display:block;margin:1rem 0 .35rem}input{box-sizing:border-box;width:100%;padding:.7rem;font:inherit}input[type=checkbox]{width:auto;margin-right:.5rem}button{margin-top:1.2rem;padding:.75rem 1rem;font:inherit}small{color:#c4cfce}</style></head><body>"
                  "<h1>Moon Phase Clock</h1><h2>Wi-Fi setup</h2>";
    if (message.length()) page += "<p>" + htmlEscape(message) + "</p>";
    page += "<form method=\"post\" action=\"/save\">"
            "<label for=\"ssid\">Wi-Fi network name</label><input id=\"ssid\" name=\"ssid\" maxlength=\"32\" required value=\"" + htmlEscape(deviceConfig.ssid) + "\">"
            "<label for=\"password\">Wi-Fi password</label><input id=\"password\" name=\"password\" type=\"password\" maxlength=\"63\" autocomplete=\"new-password\">"
            "<small>Leave blank only if the network is open.</small>"
            "<small>Location and time zone are estimated from your public IP using ipwho.is after connecting.</small>"
            "<label for=\"contactEmail\">MET Norway contact email</label><input id=\"contactEmail\" name=\"contactEmail\" type=\"email\" maxlength=\"254\" required value=\"" + htmlEscape(deviceConfig.contactEmail) + "\">"
            "<small>Sent to MET Norway to identify this app in API requests.</small>" +
            autoDimCheckbox(autoDim) +
            "<button type=\"submit\">Save and connect</button></form></body></html>";
    return page;
}

void handleConfigurationSave()
{
    DeviceConfig candidate;
    candidate.ssid = configServer.arg("ssid");
    candidate.password = configServer.arg("password");
    candidate.contactEmail = configServer.arg("contactEmail");
    candidate.autoDim = configServer.hasArg("autoDim");

    if (!isValidConfig(candidate)) {
        configServer.send(400, "text/html; charset=utf-8", configurationPage("Please check the network and contact email.", candidate.autoDim));
        return;
    }
    if (!saveDeviceConfig(candidate)) {
        configServer.send(500, "text/html; charset=utf-8", configurationPage("Could not save settings. Please try again.", candidate.autoDim));
        return;
    }

    configServer.send(200, "text/html; charset=utf-8", "<p>Settings saved. The clock is restarting and will connect to Wi-Fi.</p>");
    delay(750);
    ESP.restart();
}

void startProvisioningPortal()
{
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_AP);

    String mac = WiFi.softAPmacAddress();
    mac.replace(":", "");
    String apName = "MoonPhase-" + mac.substring(mac.length() - 4);

    if (!WiFi.softAP(apName.c_str())) {
        Serial.println("Failed to start setup access point.");
        while (true) delay(1000);
    }

    IPAddress portalIp = WiFi.softAPIP();
    dnsServer.start(DNS_PORT, "*", portalIp);
    configServer.on("/", HTTP_GET, []() {
        configServer.send(200, "text/html; charset=utf-8", configurationPage("", deviceConfig.autoDim));
    });
    configServer.on("/save", HTTP_POST, handleConfigurationSave);
    configServer.onNotFound([]() {
        configServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
        configServer.send(302, "text/plain", "");
    });
    configServer.begin();

    Serial.println("Wi-Fi setup required.");
    Serial.print("Connect to access point: ");
    Serial.println(apName);
    Serial.println("Setup access point is open; no password is required.");
    Serial.print("Open: http://");
    Serial.println(portalIp);

    while (true) {
        dnsServer.processNextRequest();
        configServer.handleClient();
        delay(2);
    }
}

bool connectToWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(deviceConfig.ssid.c_str(), deviceConfig.password.c_str());
    Serial.print("Connecting to WiFi");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(" connected.");
        return true;
    } else {
        Serial.println(" failed to connect within 20s.");
        return false;
    }
}

bool loadLocationMode()
{
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, true)) {
        Serial.println("Could not read the location mode.");
        return false;
    }
    manualLocation = preferences.getBool("manualLoc", false);
    preferences.end();
    return true;
}

bool loadManualLocation()
{
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, true)) {
        Serial.println("Could not read the manual location.");
        return false;
    }
    ManualLocation location = {};
    bool valid = preferences.getBytesLength("manualGeo") == sizeof(location) &&
                 preferences.getBytes("manualGeo", &location, sizeof(location)) == sizeof(location);
    preferences.end();

    const char *posix = nullptr;
    if (valid && memchr(location.timezoneId, '\0', sizeof(location.timezoneId)) &&
        location.timezoneId[0]) {
        posix = TzDbLookup::getPosix(location.timezoneId);
    }
    valid = valid && posix && *posix && isfinite(location.latitude) &&
            location.latitude >= -90.0 && location.latitude <= 90.0 &&
            isfinite(location.longitude) && location.longitude >= -180.0 && location.longitude <= 180.0;
    if (!valid) {
        Serial.println("Manual location is invalid; update it on the network settings page.");
        return false;
    }
    deviceConfig.latitude = location.latitude;
    deviceConfig.longitude = location.longitude;
    deviceConfig.timezoneId = location.timezoneId;
    deviceConfig.timezone = posix;
    return true;
}

bool saveManualLocation(const ManualLocation &location)
{
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, false)) return false;
    bool wasManual = preferences.getBool("manualLoc", false);
    bool saved = preferences.putBytes("manualGeo", &location, sizeof(location)) == sizeof(location) &&
                 (wasManual || preferences.putBool("manualLoc", true) > 0);
    preferences.end();
    return saved;
}

bool useAutomaticLocation()
{
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, false)) return false;
    bool saved = preferences.putBool("manualLoc", false) > 0;
    preferences.end();
    return saved;
}

bool validSettingsHost()
{
    String host = configServer.hostHeader();
    String address = WiFi.localIP().toString();
    return host == address || host == address + ":80";
}

String settingsPage(const String &message, const String &mode, const String &latitude,
                    const String &longitude, const String &timezoneId, bool autoDim)
{
    String page = "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                  "<title>Moon Phase Clock Location</title><style>body{font:16px sans-serif;max-width:32rem;margin:2rem auto;padding:0 1rem;background:#101820;color:#f2f4f3}"
                  "label{display:block;margin:1rem 0 .35rem}input,select{box-sizing:border-box;width:100%;padding:.7rem;font:inherit}input[type=checkbox]{width:auto;margin-right:.5rem}"
                  "button{margin-top:1.2rem;padding:.75rem 1rem;font:inherit}small{color:#c4cfce}</style></head><body>"
                  "<h1>Moon Phase Clock</h1><h2>Location and time zone</h2>";
    if (message.length()) page += "<p>" + htmlEscape(message) + "</p>";
    page += "<p>Current mode: ";
    page += manualLocation ? "Manual" : (geolocationPending ? "Automatic (lookup pending; values may be cached)" : "Automatic");
    page += locationValid ? ".</p>" : ". Location is not available.</p>";
    page += "<form method=\"post\" action=\"/location\">"
            "<input type=\"hidden\" name=\"token\" value=\"" + htmlEscape(settingsToken) + "\">"
            "<label for=\"mode\">Location source</label><select id=\"mode\" name=\"mode\">"
            "<option value=\"auto\"" + String(mode == "auto" ? " selected" : "") + ">Automatic IP-based location</option>"
            "<option value=\"manual\"" + String(mode == "manual" ? " selected" : "") + ">Manual location and time zone</option></select>"
            "<label for=\"latitude\">Latitude</label><input id=\"latitude\" name=\"latitude\" inputmode=\"decimal\" value=\"" + htmlEscape(latitude) + "\">"
            "<label for=\"longitude\">Longitude</label><input id=\"longitude\" name=\"longitude\" inputmode=\"decimal\" value=\"" + htmlEscape(longitude) + "\">"
            "<label for=\"timezone\">IANA time zone</label><input id=\"timezone\" name=\"timezone\" maxlength=\"63\" value=\"" + htmlEscape(timezoneId) + "\">"
            "<small>For manual mode, enter coordinates and a time zone such as Australia/Sydney or America/Los_Angeles. Automatic mode ignores these fields.</small>"
            "<br><button type=\"submit\">Save and restart</button></form>"
            "<h2>Display</h2><form method=\"post\" action=\"/autodim\">"
            "<input type=\"hidden\" name=\"token\" value=\"" + htmlEscape(settingsToken) + "\">" +
            autoDimCheckbox(autoDim) +
            "<button type=\"submit\">Save display setting</button></form></body></html>";
    return page;
}

String currentSettingsPage(const String &message, bool autoDim)
{
    return settingsPage(message, manualLocation ? "manual" : "auto",
                        locationValid ? String(deviceConfig.latitude, 6) : "",
                        locationValid ? String(deviceConfig.longitude, 6) : "",
                        locationValid ? deviceConfig.timezoneId : "", autoDim);
}

void handleSettingsSave()
{
    configServer.sendHeader("Cache-Control", "no-store");
    configServer.sendHeader("X-Frame-Options", "DENY");
    configServer.sendHeader("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
    if (!validSettingsHost() || configServer.arg("token") != settingsToken) {
        configServer.send(403, "text/plain", "Invalid settings request.");
        return;
    }

    String mode = configServer.arg("mode");
    String latitude = configServer.arg("latitude");
    String longitude = configServer.arg("longitude");
    String timezoneId = configServer.arg("timezone");
    bool saved = false;
    if (mode == "auto") {
        saved = useAutomaticLocation();
    } else if (mode == "manual") {
        char *latEnd = nullptr;
        char *lonEnd = nullptr;
        double lat = strtod(latitude.c_str(), &latEnd);
        double lon = strtod(longitude.c_str(), &lonEnd);
        const char *posix = timezoneId.length() <= 63 ? TzDbLookup::getPosix(timezoneId.c_str()) : nullptr;
        if (latEnd == latitude.c_str() || *latEnd != '\0' ||
            lonEnd == longitude.c_str() || *lonEnd != '\0' ||
            !isfinite(lat) || lat < -90.0 || lat > 90.0 ||
            !isfinite(lon) || lon < -180.0 || lon > 180.0 || !posix || !*posix) {
            configServer.send(400, "text/html; charset=utf-8",
                              settingsPage("Enter valid coordinates and a supported IANA time zone.", mode, latitude, longitude, timezoneId, deviceConfig.autoDim));
            return;
        }
        ManualLocation location = {};
        location.latitude = lat;
        location.longitude = lon;
        memcpy(location.timezoneId, timezoneId.c_str(), timezoneId.length() + 1);
        saved = saveManualLocation(location);
    } else {
        configServer.send(400, "text/html; charset=utf-8",
                          settingsPage("Select a valid location source.", mode, latitude, longitude, timezoneId, deviceConfig.autoDim));
        return;
    }
    if (!saved) {
        configServer.send(500, "text/html; charset=utf-8",
                          settingsPage("Could not save settings. Please try again.", mode, latitude, longitude, timezoneId, deviceConfig.autoDim));
        return;
    }

    configServer.send(200, "text/html; charset=utf-8", "<p>Settings saved. The clock is restarting.</p>");
    delay(750);
    ESP.restart();
}

void handleAutoDimSave()
{
    configServer.sendHeader("Cache-Control", "no-store");
    configServer.sendHeader("X-Frame-Options", "DENY");
    configServer.sendHeader("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
    if (!validSettingsHost() || configServer.arg("token") != settingsToken) {
        configServer.send(403, "text/plain", "Invalid settings request.");
        return;
    }

    bool enabled = configServer.hasArg("autoDim");
    if (!saveAutoDim(enabled)) {
        configServer.send(500, "text/html; charset=utf-8", currentSettingsPage("Could not save display setting. Please try again.", enabled));
        return;
    }
    deviceConfig.autoDim = enabled;
    updateAutoDim();
    configServer.send(200, "text/html; charset=utf-8", currentSettingsPage("Display setting saved.", enabled));
}

void startSettingsServer()
{
    char token[33];
    snprintf(token, sizeof(token), "%08lx%08lx%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());
    settingsToken = token;
    configServer.on("/", HTTP_GET, []() {
        if (!validSettingsHost()) {
            configServer.send(403, "text/plain", "Use the clock's IP address.");
            return;
        }
        configServer.sendHeader("Cache-Control", "no-store");
        configServer.sendHeader("X-Frame-Options", "DENY");
        configServer.sendHeader("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
        configServer.send(200, "text/html; charset=utf-8", currentSettingsPage("", deviceConfig.autoDim));
    });
    configServer.on("/location", HTTP_POST, handleSettingsSave);
    configServer.on("/autodim", HTTP_POST, handleAutoDimSave);
    configServer.onNotFound([]() {
        configServer.send(404, "text/plain", "Not found.");
    });
    configServer.begin();
}

void showIpAddress()
{
    String address = WiFi.localIP().toString();
    String text = "Settings address:\n" + address;
    lv_label_set_text(ui_ip_label, text.c_str());
    lv_obj_center(ui_ip_label);
    lv_obj_clear_flag(ui_ip_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui_ip_overlay);
    lv_refr_now(NULL);
    ipShownAt = millis();
    showingIp = true;
    Serial.printf("Settings: http://%s/\n", address.c_str());
}

void updateIpDisplay()
{
    if (showingIp && millis() - ipShownAt >= 10000UL) {
        lv_obj_add_flag(ui_ip_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        showingIp = false;
    }
}

bool loadCachedLocation()
{
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, true)) {
        Serial.println("Could not read cached location.");
        return false;
    }

    bool valid = preferences.getBool("geoValid", false) &&
                 preferences.getString("geoNetwork", "") == deviceConfig.ssid &&
                 preferences.isKey("geoLat") && preferences.isKey("geoLon") &&
                 preferences.isKey("geoTz");
    if (valid) {
        deviceConfig.latitude = preferences.getDouble("geoLat", NAN);
        deviceConfig.longitude = preferences.getDouble("geoLon", NAN);
        deviceConfig.timezone = preferences.getString("geoTz", "");
        deviceConfig.timezoneId = preferences.getString("geoIana", "");
        valid = isfinite(deviceConfig.latitude) && deviceConfig.latitude >= -90.0 &&
                deviceConfig.latitude <= 90.0 && isfinite(deviceConfig.longitude) &&
                deviceConfig.longitude >= -180.0 && deviceConfig.longitude <= 180.0 &&
                deviceConfig.timezone.length() > 0;
        if (!valid) Serial.println("Cached location is invalid.");
    }
    preferences.end();
    return valid;
}

bool saveCachedLocation()
{
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, false)) return false;

    bool saved = preferences.putBool("geoValid", false) > 0 &&
                 preferences.putString("geoNetwork", deviceConfig.ssid) > 0 &&
                 preferences.putDouble("geoLat", deviceConfig.latitude) > 0 &&
                 preferences.putDouble("geoLon", deviceConfig.longitude) > 0 &&
                 preferences.putString("geoTz", deviceConfig.timezone) > 0 &&
                 preferences.putString("geoIana", deviceConfig.timezoneId) > 0 &&
                 preferences.putBool("geoValid", true) > 0;
    preferences.end();
    return saved;
}

bool fetchLocation(DeviceConfig &result, uint64_t &retryMs)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Geolocation failed: Wi-Fi is not connected.");
        return false;
    }
    time_t now;
    time(&now);
    if (now < 1672531200) {
        Serial.println("Geolocation waiting for NTP time to verify HTTPS.");
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(GEOLOCATION_CA);
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    http.useHTTP10(true);
    if (!http.begin(client, GEOLOCATION_URL)) {
        Serial.println("Failed to initialize geolocation request.");
        return false;
    }
    const char *headers[] = {"Retry-After"};
    http.collectHeaders(headers, 1);
    int status = http.GET();
    if (status != HTTP_CODE_OK) {
        Serial.printf("Geolocation request failed, HTTP code: %d (%s)\n",
                      status, http.errorToString(status).c_str());
        if (status == 429) {
            retryMs = 24ULL * 60ULL * 60ULL * 1000ULL;
            String header = http.header("Retry-After");
            char *end = nullptr;
            errno = 0;
            unsigned long long seconds = strtoull(header.c_str(), &end, 10);
            if (header.length() && header[0] >= '0' && header[0] <= '9' &&
                end != header.c_str() && *end == '\0' && errno != ERANGE &&
                seconds > 0 && seconds <= UINT64_MAX / 1000ULL - 86400ULL) {
                retryMs = seconds * 1000ULL;
                if (retryMs < GEOLOCATION_RETRY_MS) retryMs = GEOLOCATION_RETRY_MS;
            }
        }
        http.end();
        return false;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, http.getStream());
    http.end();
    if (error) {
        Serial.printf("Geolocation JSON parse failed: %s\n", error.c_str());
        return false;
    }
    if (!doc["success"].is<bool>() || !doc["success"].as<bool>()) {
        Serial.printf("Geolocation service returned an error: %s\n", doc["message"] | "unknown error");
        return false;
    }
    JsonVariantConst latitude = doc["latitude"];
    JsonVariantConst longitude = doc["longitude"];
    const char *iana = doc["timezone"]["id"] | (const char *)nullptr;
    if (!latitude.is<double>() || !longitude.is<double>() || !iana || !*iana) {
        Serial.println("Geolocation response is missing coordinates or a time zone.");
        return false;
    }
    double lat = latitude.as<double>();
    double lon = longitude.as<double>();
    if (!isfinite(lat) || lat < -90.0 || lat > 90.0 ||
        !isfinite(lon) || lon < -180.0 || lon > 180.0) {
        Serial.println("Geolocation response contains invalid coordinates.");
        return false;
    }
    const char *posix = TzDbLookup::getPosix(iana);
    if (!posix || !*posix) {
        Serial.printf("Unsupported geolocation time zone: %s\n", iana);
        return false;
    }
    Serial.printf("Geolocation: %s, time zone %s\n", doc["city"] | "(unknown city)", iana);
    result.latitude = lat;
    result.longitude = lon;
    result.timezone = posix;
    result.timezoneId = iana;
    return true;
}

bool applyLocation(const DeviceConfig &config)
{
    if (setenv("TZ", config.timezone.c_str(), 1) != 0) {
        Serial.println("Could not apply the geolocation time zone.");
        return false;
    }
    tzset();
    lv_img_set_pivot(ui_img_moon, 50, 50);
    lv_img_set_angle(ui_img_moon, config.latitude < 0.0 ? 1800 : 0);
    return true;
}

void updateGeolocation()
{
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000ULL;
    if (!geolocationPending || now < nextGeolocationAttemptMs) return;

    uint64_t retryMs = GEOLOCATION_RETRY_MS;
    DeviceConfig result = deviceConfig;
    if (fetchLocation(result, retryMs) && applyLocation(result)) {
        bool changed = !locationValid || deviceConfig.latitude != result.latitude ||
                       deviceConfig.longitude != result.longitude || deviceConfig.timezone != result.timezone ||
                       deviceConfig.timezoneId != result.timezoneId;
        deviceConfig = result;
        locationValid = true;
        geolocationPending = false;
        updateAutoDim();
        if (changed && !saveCachedLocation()) {
            Serial.println("Could not save the geolocation cache.");
        }
        moonDataValid = false;
        updateClockLabels();
        updateMoonData();
        lastMoonMillis = millis();
        return;
    }
    nextGeolocationAttemptMs = (uint64_t)esp_timer_get_time() / 1000ULL + retryMs;
    Serial.printf("Geolocation will retry in %llu seconds.\n", retryMs / 1000ULL);
    if (!locationValid) setMoonPhaseLabel("Location error");
}

bool waitForTimeSync(uint32_t timeoutMs)
{
    Serial.print("Waiting for NTP time sync");
    uint32_t start = millis();
    time_t now;
    struct tm timeinfo;

    while (millis() - start < timeoutMs) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2023 - 1900)) {
            Serial.println(" synced.");
            return true;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println(" timed out -- will keep retrying in the background.");
    return false;
}

void updateClockLabels()
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2023 - 1900)) {
        // Time not synced yet
        return;
    }

    char timeStr[9]; // "HH:MM:SS"
    int displayHour = timeinfo.tm_hour % 12;
    if (displayHour == 0) displayHour = 12;
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", displayHour, timeinfo.tm_min, timeinfo.tm_sec);

    char dateStr[11]; // "13 Aug 26"
    strftime(dateStr, sizeof(dateStr), "%d %b %y", &timeinfo);

    lv_label_set_text(ui_time, timeStr);
    lv_label_set_text(ui_time_period, timeinfo.tm_hour < 12 ? "am" : "pm");
    lv_obj_align_to(ui_time_period, ui_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -2);
    lv_label_set_text(ui_date, dateStr);
    lv_obj_invalidate(ui_time);
    lv_obj_invalidate(ui_time_period);
    lv_obj_invalidate(ui_date);
    lv_refr_now(NULL);
}

void setMoonImage(int index)
{
    if (index < 0) index = 0;
    if (index > 29) index = 29;
    lv_img_set_src(ui_img_moon, moon_frames[index]);
    lv_obj_invalidate(ui_img_moon);
    lv_refr_now(NULL); 
}

void playMoonIntroAnimation()
{
    const uint16_t frameDelayMs = 100; 
    for (int i = 0; i < 30; i++) {
        setMoonImage(i);
        delay(frameDelayMs);
    }
}

String getMoonPhase(double age)
{
    if (age < 1.84566) return "New Moon";
    else if (age < 5.53699) return "Waxing Crescent";
    else if (age < 9.22831) return "First Quarter";
    else if (age < 12.91963) return "Waxing Gibbous";
    else if (age < 16.61096) return "Full Moon";
    else if (age < 20.30228) return "Waning Gibbous";
    else if (age < 23.99361) return "Last Quarter";
    else return "Waning Crescent";
}

int getMoonImageIndex(double age)
{

    int idx = (int)(age / 29.53 * 30);
    if (idx > 29) idx = 29;
    if (idx < 0) idx = 0;
    return idx;
}

void setMoonPhaseLabel(const char *text)
{
    lv_label_set_text(ui_phase, text);
    lv_obj_invalidate(ui_phase);
    lv_refr_now(NULL);
}

// Fetches today's moon phase and moonrise/moonset from MET Norway's free
// Sunrise/Moon API (no API key needed).
// Docs: https://api.met.no/weatherapi/sunrise/3.0/documentation
void updateMoonData()
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected. Skipping moon data fetch.");
        setMoonPhaseLabel("API Fail");
        return;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2023 - 1900)) {
        Serial.println("Time not set yet. Skipping moon data fetch.");
        setMoonPhaseLabel("API Fail");
        return;
    }

    char dateStr[11]; // "YYYY-MM-DD"
    strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);

    // Build the "+HH:MM" / "-HH:MM" UTC offset the API wants. We compute
    // this ourselves (rather than relying on the BSD tm_gmtoff extension,
    // which isn't available on every ESP32 core/toolchain combo) by
    // comparing local wall-clock time against UTC for the same instant.
    struct tm utcTimeinfo;
    gmtime_r(&now, &utcTimeinfo);
    struct tm localCopy = timeinfo;
    localCopy.tm_isdst = 0;
    utcTimeinfo.tm_isdst = 0;
    long gmtOffset = (long)difftime(mktime(&localCopy), mktime(&utcTimeinfo));

    char offsetStr[8];
    snprintf(offsetStr, sizeof(offsetStr), "%c%02ld:%02ld",
             gmtOffset < 0 ? '-' : '+',
             labs(gmtOffset) / 3600,
             (labs(gmtOffset) % 3600) / 60);

    String url = "https://api.met.no/weatherapi/sunrise/3.0/moon?lat=";
    url += String(deviceConfig.latitude, 4);
    url += "&lon=";
    url += String(deviceConfig.longitude, 4);
    url += "&date=";
    url += dateStr;
    url += "&offset=";
    if (offsetStr[0] == '+') {
        url += "%2B";
        url += offsetStr + 1;
    } else {
        url += offsetStr;
    }

    Serial.print("Fetching moon phase and moonrise/set: ");
    Serial.println(url);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, url)) {
        Serial.println("Failed to initialize moon data request.");
        setMoonPhaseLabel("API Fail");
        return;
    }
    // Identify this app with the contact address supplied in the setup portal.
    // See https://api.met.no/doc/TermsOfService
    http.setUserAgent("MoonPhaseClock/1.0 (" + deviceConfig.contactEmail + ")");

    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();

        DynamicJsonDocument doc(2048);
        DeserializationError err = deserializeJson(doc, payload);
        if (err) {
            Serial.print("Moon data JSON parse failed: ");
            Serial.println(err.c_str());
            setMoonPhaseLabel("API Fail");
            http.end();
            return;
        }

        JsonVariantConst phaseValue = doc["properties"]["moonphase"];
        if (phaseValue.is<double>()) {
            double phase = phaseValue.as<double>();
            if (isfinite(phase) && phase >= 0.0 && phase <= 360.0) {
                double age = (phase == 360.0 ? 0.0 : phase) / 360.0 * 29.53;
                Serial.printf("Daily moon phase: %.2f degrees\n", phase);
                String phaseName = getMoonPhase(age);
                setMoonPhaseLabel(phaseName.c_str());
                setMoonImage(getMoonImageIndex(age));
                moonDataValid = true;
            } else {
                Serial.println("Invalid moon phase in MET Norway response.");
                setMoonPhaseLabel("API Fail");
            }
        } else {
            Serial.println("Missing or non-numeric moon phase in MET Norway response.");
            setMoonPhaseLabel("API Fail");
        }

        const char *riseTime = doc["properties"]["moonrise"]["time"] | (const char *)nullptr;
        const char *setTime  = doc["properties"]["moonset"]["time"]  | (const char *)nullptr;

        // Times look like "2026-09-07T06:15+10:00" -- since we asked for
        // the local offset, the HH:MM we want sits right after the 'T'.
        char riseStr[6] = "--:--";
        char setStr[6]  = "--:--";
        if (riseTime && strlen(riseTime) >= 16) {
            memcpy(riseStr, riseTime + 11, 5);
            riseStr[5] = '\0';
        }
        if (setTime && strlen(setTime) >= 16) {
            memcpy(setStr, setTime + 11, 5);
            setStr[5] = '\0';
        }
        // Note: moonrise or moonset can legitimately be missing/null for a
        // given local day (it happens roughly once a fortnight) -- that's
        // when you'll see "--:--" rather than a fetch error.

        char line[40];
        snprintf(line, sizeof(line), "   %s\n   %s", riseStr, setStr);
        lv_label_set_text(ui_moon_times, line);
        lv_obj_invalidate(ui_moon_times);
        lv_refr_now(NULL);

    } else {
        Serial.printf("Failed to fetch moon data, HTTP code: %d (%s)\n",
                      httpCode, http.errorToString(httpCode).c_str());
        setMoonPhaseLabel("API Fail");
    }

    http.end();
}
