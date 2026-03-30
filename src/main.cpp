/**
 * M5RadioStream — WiFi Internet Radio for M5Stack Core1 Basic
 *
 * Ported from winRadio by Volos Projects (Waveshare ESP32-S3 + ST7789 240x240)
 * Target: M5Stack Core1 Basic (ESP32, ILI9341 320x240, 3 physical buttons)
 *
 * Button mapping (normal mode):
 *   BtnA (left)   = Open sound settings (Bass / Treble / Volume)
 *   BtnB (middle) = Cycle station
 *   BtnC (right)  = Cycle volume
 *
 * Sound settings mode (BtnA to enter/exit):
 *   BtnA = Exit back to radio
 *   BtnB = Select next parameter (Volume → Bass → Treble → ...)
 *   BtnC = Increase selected parameter (wraps at max → min)
 *
 * AUDIO: M5Stack Core1 Basic uses the ESP32 internal DAC on GPIO25.
 *        Audio(true) selects DAC mode — no external I2S amp needed.
 *        Call M5.Speaker.end() first so LEDC doesn't fight the DAC.
 *
 * DISPLAY: No PSRAM on Core1 Basic — draw directly to M5.Lcd inside
 *          startWrite/endWrite. Static background drawn once (setupUI);
 *          drawDynamic() only repaints regions that actually change,
 *          eliminating the full-screen wipe that caused flicker.
 *          Only the small 310x16 ticker uses a sprite.
 *
 * WiFi credentials stored in NVS via setup portal.
 * Hold BtnA during the 3-second boot window to re-enter setup.
 */

#include <M5Stack.h>
#include <WiFiMulti.h>
#include <Audio.h>
#include "Portal.h"

#define SCREEN_W  320
#define SCREEN_H  240

#define ns 8
// ROKiT Radio Network — OTR/classic streams, all 48 kbps MP3
String stations[ns] = {
    "http://streaming06.liveboxstream.uk:8256/stream",  // 1940s Radio
    "http://streaming05.liveboxstream.uk:8043/stream",  // American Classics
    "http://streaming06.liveboxstream.uk:8027/stream",  // Jazz Central
    "http://streaming06.liveboxstream.uk:8150/stream",  // Comedy Gold
    "http://streaming05.liveboxstream.uk:8039/stream",  // Crime Radio
    "http://streaming06.liveboxstream.uk:8180/stream",  // Nostalgia Lane
    "http://streaming05.liveboxstream.uk:8009/stream",  // British Comedy
    "http://streaming05.liveboxstream.uk:8110/stream",  // Science Fiction
};
String stationNames[ns] = {
    "1940s Radio",
    "American Classics",
    "Jazz Central",
    "Comedy Gold",
    "Crime Radio",
    "Nostalgia Lane",
    "British Comedy",
    "Science Fiction",
};

TFT_eSprite sprite2(&M5.Lcd);  // scrolling ticker only (~10 KB)

Audio     audio(true, 3);       // internal DAC, both channels (GPIO25/26)
WiFiMulti wifiMulti;

// Audio runs on its own FreeRTOS task so draw2() never starves audio.loop()
void audioTask(void *param) {
    while (true) {
        audio.loop();
        vTaskDelay(1);
    }
}
TaskHandle_t audioTaskHandle = NULL;

String curStation  = "";
String songPlaying = "";
long   bitrate     = 0;
bool   connected   = false;
int    songposition = -310;
float  voltage     = 4.20;
int    batLevel    = 0;

bool  canDraw = false;
int   rssi    = 0;

int    chosen = 0;
int    volume = 2;   // 1-5, maps to audio.setVolume(volume * 4)

unsigned short grays[18];
unsigned short gray;
unsigned short light;
int g[14] = {0};

// Sound settings state
bool   inSettings   = false;
int    settingSel   = 0;    // 0=Volume, 1=Bass, 2=Treble
int8_t settingBass  = 0;
int8_t settingTreble = 0;

// ---------------------------------------------------------------------------
void setupUI();   // forward declaration — defined after setup()

void setup() {
    M5.begin(true, false, true, false);
    M5.Speaker.end();   // free GPIO25 from LEDC so DAC can take over
    M5.Lcd.fillScreen(TFT_BLACK);

    int co = 214;
    for (int i = 0; i < 18; i++) {
        grays[i] = M5.Lcd.color565(co, co, co + 40);
        co -= 13;
    }

    sprite2.setColorDepth(16);
    sprite2.createSprite(310, 16);
    sprite2.setTextFont(2);
    sprite2.setTextColor(grays[0], TFT_BLACK);

    rdLoadSettings();

    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_GREEN);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.print("M5 Radio Stream");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.setCursor(4, 40);
    M5.Lcd.print("Hold BtnA for WiFi setup...");

    bool enterPortal = !rd_has_settings;
    unsigned long bootStart = millis();
    while (millis() - bootStart < 3000) {
        M5.update();
        if (M5.BtnA.isPressed()) { enterPortal = true; break; }
        delay(50);
    }

    if (enterPortal) {
        rdInitPortal();
        while (!portalDone) { rdRunPortal(); delay(1); }
        rdClosePortal();
    }

    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_GREEN);
    M5.Lcd.setCursor(2, 20);
    M5.Lcd.println("Connecting to WiFi...");

    WiFi.mode(WIFI_STA);
    wifiMulti.addAP(rd_wifi_ssid, rd_wifi_pass);
    wifiMulti.run();
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true);
        wifiMulti.run();
    }

    audio.setVolume(volume * 4);
    audio.connecttohost(stations[0].c_str());

    // Pin audio task to core 0 (alongside WiFi stack), high priority
    xTaskCreatePinnedToCore(audioTask, "audioT", 8192, NULL, 2,
                            &audioTaskHandle, 0);

    setupUI();   // draw static background once — no full-screen wipe in loop
    canDraw = true;
}

// ---------------------------------------------------------------------------
// setupUI — draw all static (non-changing) UI elements once.
// Call at startup and when returning from the settings screen.
void setupUI() {
    gray  = grays[16];
    light = grays[12];

    M5.Lcd.startWrite();

    // Background + panels
    M5.Lcd.fillRect(0, 0, SCREEN_W, SCREEN_H, gray);
    M5.Lcd.fillRect(4, 20, 205, 172, TFT_BLACK);
    M5.Lcd.drawRect(4, 20, 205, 172, light);
    M5.Lcd.fillRect(215, 20, 100, 60, TFT_BLACK);
    M5.Lcd.drawRect(215, 20, 100, 60, light);
    M5.Lcd.fillRect(215, 176, 100, 16, TFT_BLACK);
    M5.Lcd.drawRect(215, 176, 100, 16, light);

    // Volume track (yellow line — knob drawn in drawDynamic)
    M5.Lcd.fillRoundRect(215, 140, 100, 3, 2, TFT_YELLOW);

    // Song ticker panel
    M5.Lcd.fillRect(4, 212, 312, 18, TFT_BLACK);
    M5.Lcd.drawRect(4, 212, 312, 18, light);

    // Divider bar between station list and right panel
    M5.Lcd.fillRect(209, 20, 5, 172, grays[11]);
    M5.Lcd.fillRect(209, 20, 5, 20,  grays[2]);
    M5.Lcd.fillRect(211, 24, 1, 12,  grays[16]);

    // Orange accent lines
    M5.Lcd.fillRect(4,   7,  205, 3, TFT_ORANGE);
    M5.Lcd.fillRect(245, 5,  45,  3, TFT_ORANGE);
    M5.Lcd.fillRect(215, 194, 100, 1, TFT_ORANGE);
    M5.Lcd.fillRect(245, 11, 45,  3, grays[6]);

    // Outer border + bottom shadow
    M5.Lcd.drawRect(0, 0, SCREEN_W - 1, SCREEN_H - 1, light);
    M5.Lcd.fillRect(5, 234, 310, 2, grays[13]);

    // Static labels
    M5.Lcd.setTextFont(2);
    M5.Lcd.setTextColor(grays[1], gray);
    M5.Lcd.drawString(" STATIONS ", 60, 2);
    M5.Lcd.drawString("WEB", 215, 2);

    M5.Lcd.setTextColor(grays[0], gray);
    M5.Lcd.drawString("INTERNET", 215, 86);
    M5.Lcd.setTextColor(TFT_RED, gray);
    M5.Lcd.drawString("RADIO", 215, 102);

    M5.Lcd.setTextColor(grays[6], gray);
    M5.Lcd.drawString("SONG PLAYING", 6, 200);
    M5.Lcd.drawString("VOLUME", 215, 124);

    M5.Lcd.setTextColor(grays[11], gray);
    M5.Lcd.drawString("VOLOS 2026", 155, 200);

    // WIFI stack text (decorative — status dots drawn in drawDynamic)
    M5.Lcd.setTextColor(grays[10], TFT_BLACK);
    M5.Lcd.drawString("W", 220, 24);
    M5.Lcd.drawString("I", 220, 34);
    M5.Lcd.drawString("F", 220, 44);
    M5.Lcd.drawString("I", 220, 54);

    // Button labels: SET / STA / VOL
    M5.Lcd.setTextColor(grays[16], grays[5]);
    const char *btnLabels[3] = {"SET", "STA", "VOL"};
    for (int i = 0; i < 3; i++) {
        M5.Lcd.fillRoundRect(215 + (i * 32), 152, 28, 18, 4, grays[5]);
        M5.Lcd.drawString(btnLabels[i], 216 + (i * 32), 154);
    }

    M5.Lcd.endWrite();
}

// ---------------------------------------------------------------------------
// drawDynamic — repaint only the regions that change each cycle.
// Never fills the full screen, so there is no visible flicker.
void drawDynamic() {
    M5.Lcd.startWrite();

    // --- Station list (erase only the list area, then redraw) ---
    M5.Lcd.fillRect(5, 21, 203, 170, TFT_BLACK);
    M5.Lcd.setTextFont(1);
    for (int i = 0; i < ns; i++) {
        M5.Lcd.setTextColor(i == chosen ? TFT_GREEN : TFT_DARKGREEN, TFT_BLACK);
        M5.Lcd.drawString(stationNames[i], 10, 26 + (i * 19));
    }

    // --- VU meter bars (erase then redraw animated bars) ---
    M5.Lcd.fillRect(227, 24, 60, 48, TFT_BLACK);
    for (int i = 0; i < 12; i++) {
        if (connected) g[i] = random(1, 5);
        for (int j = 0; j < g[i]; j++)
            M5.Lcd.fillRect(227 + (i * 5), 71 - j * 4, 4, 3, grays[4]);
    }

    // --- WiFi status dots ---
    M5.Lcd.fillRect(229, 24, 5, 10, connected ? TFT_GREEN : TFT_RED);
    M5.Lcd.fillRect(229, 37, 5, 10, connected ? TFT_GREEN : TFT_DARKGREEN);

    // --- RSSI + voltage (erase right portion of info panel) ---
    M5.Lcd.setTextFont(2);
    M5.Lcd.fillRect(238, 24, 75, 28, TFT_BLACK);
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Lcd.drawString("RSSI:" + String(rssi), 238, 24);
    M5.Lcd.drawString(String(voltage, 2) + "V", 238, 37);

    // --- Battery icon ---
    M5.Lcd.drawRect(265, 36, 17, 10, TFT_GREEN);
    M5.Lcd.fillRect(267, 38, 13, 6, TFT_BLACK);
    M5.Lcd.fillRect(267, 38, batLevel, 6, TFT_GREEN);
    M5.Lcd.fillRect(282, 39, 2, 4, TFT_GREEN);

    // --- Bitrate ---
    M5.Lcd.fillRect(216, 177, 98, 14, TFT_BLACK);
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Lcd.drawString("BITRATE " + String(bitrate), 219, 180);

    // --- Volume knob (erase knob track area, redraw knob position) ---
    M5.Lcd.fillRect(215, 136, 100, 12, gray);
    M5.Lcd.fillRoundRect(200 + (volume * 15), 137, 14, 8, 2, grays[2]);
    M5.Lcd.fillRoundRect(203 + (volume * 15), 139, 8,  4, 2, grays[10]);

    M5.Lcd.endWrite();

    canDraw = false;
}

void draw3() {
    songposition--;
    if (songposition < -310) songposition = 310;
    sprite2.fillSprite(TFT_BLACK);
    sprite2.drawString(songPlaying, songposition, 0);
    sprite2.pushSprite(5, 213);
}

// ---------------------------------------------------------------------------
// drawSettings — full-screen sound settings overlay.
// Shows Volume, Bass, Treble with a progress bar each.
void drawSettings() {
    const char *labels[3]  = { "Volume", "Bass  ", "Treble" };
    int   values[3]  = { volume * 4, settingBass, settingTreble };
    int   mins[3]    = { 4, -6, -6 };
    int   maxs[3]    = { 20,  6,  6 };

    M5.Lcd.startWrite();
    M5.Lcd.fillRect(0, 0, SCREEN_W, SCREEN_H, TFT_BLACK);

    M5.Lcd.setTextFont(2);
    M5.Lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
    M5.Lcd.drawString("== SOUND SETTINGS ==", 55, 10);
    M5.Lcd.drawFastHLine(0, 30, SCREEN_W, TFT_ORANGE);

    for (int i = 0; i < 3; i++) {
        bool sel = (i == settingSel);
        int  y   = 48 + i * 55;
        uint16_t bg = sel ? 0x1082 : TFT_BLACK;

        M5.Lcd.fillRect(20, y - 4, 280, 46, bg);
        M5.Lcd.setTextColor(sel ? TFT_GREEN : TFT_WHITE, bg);
        M5.Lcd.drawString(labels[i], 30, y);
        M5.Lcd.drawString(String(values[i]), 230, y);

        // Progress bar
        int pos = map(values[i], mins[i], maxs[i], 0, 200);
        M5.Lcd.drawRect(30, y + 20, 202, 10, sel ? TFT_GREEN : grays[12]);
        M5.Lcd.fillRect(31, y + 21, 200, 8, TFT_BLACK);
        M5.Lcd.fillRect(31, y + 21, pos,  8, sel ? TFT_GREEN : grays[8]);
    }

    M5.Lcd.drawFastHLine(0, SCREEN_H - 34, SCREEN_W, TFT_ORANGE);
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextColor(grays[6], TFT_BLACK);
    M5.Lcd.drawString("BtnA: back    BtnB: select    BtnC: +", 30, SCREEN_H - 22);

    M5.Lcd.endWrite();
}

void measureBatt() {
    int level = M5.Power.getBatteryLevel();
    voltage  = 3.0f + (level / 100.0f) * 1.2f;
    batLevel = (int)((level / 100.0f) * 13.0f);
}

// ---------------------------------------------------------------------------

void loop() {
    M5.update();

    static unsigned long lastRSSI  = 0;
    static unsigned long lastSlide = 0;
    static unsigned long lastDraw  = 0;

    // ---- Sound settings mode ----
    if (inSettings) {
        if (M5.BtnA.wasPressed()) {
            inSettings = false;
            setupUI();          // restore static background
            canDraw = true;
        }
        if (M5.BtnB.wasPressed()) {
            settingSel = (settingSel + 1) % 3;
            drawSettings();
        }
        if (M5.BtnC.wasPressed()) {
            if (settingSel == 0) {
                // Volume: steps of 4 across 4-20 range
                volume++;
                if (volume > 5) volume = 1;
                audio.setVolume(volume * 4);
            } else if (settingSel == 1) {
                settingBass++;
                if (settingBass > 6) settingBass = -6;
                audio.setTone(settingBass, 0, settingTreble);
            } else {
                settingTreble++;
                if (settingTreble > 6) settingTreble = -6;
                audio.setTone(settingBass, 0, settingTreble);
            }
            drawSettings();
        }
        vTaskDelay(5);
        return;
    }

    // ---- Normal radio mode ----
    if (millis() - lastRSSI > 240) {
        lastRSSI = millis();
        rssi = WiFi.RSSI();
        measureBatt();
        canDraw = true;
        connected = (WiFi.status() == WL_CONNECTED);
        if (!connected) songPlaying = "WIFI NOT CONNECTED";
    }

    if (millis() - lastSlide > 30) {
        lastSlide = millis();
        draw3();
    }

    if (M5.BtnA.wasPressed()) {
        inSettings = true;
        drawSettings();
    }

    if (M5.BtnB.wasPressed()) {
        chosen++;
        if (chosen == ns) chosen = 0;
        audio.connecttohost(stations[chosen].c_str());
        canDraw = true;
    }

    if (M5.BtnC.wasPressed()) {
        volume++;
        if (volume > 5) volume = 1;
        audio.setVolume(volume * 4);
        canDraw = true;
    }

    // drawDynamic updates only changed regions — no full-screen wipe, no flicker
    if (canDraw && millis() - lastDraw > 800) {
        lastDraw = millis();
        drawDynamic();
    }

    vTaskDelay(5);
}

// ---------------------------------------------------------------------------

void audio_info(const char *info) {
    Serial.print("info        "); Serial.println(info);
}
void audio_id3data(const char *info) {
    Serial.print("id3data     "); Serial.println(info);
}
void audio_showstation(const char *info) {
    Serial.print("station     "); Serial.println(info);
    curStation = info;
    canDraw = true;
}
void audio_showstreamtitle(const char *info) {
    Serial.print("streamtitle "); Serial.println(info);
    songPlaying = info;
    canDraw = true;
}
void audio_bitrate(const char *info) {
    Serial.print("bitrate     "); Serial.println(info);
    bitrate = String(info).toInt() / 1000;
}
