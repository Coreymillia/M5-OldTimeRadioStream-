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

// ─── Shortwave skin palette ───────────────────────────────────────────────────
#define SW_AMBER    0xFAE0   // warm amber — labels, dividers, active accents
#define SW_AMBER_D  0x7100   // dim amber — inactive elements
#define SW_HDR_BG   0x1082   // dark charcoal — header background
#define SW_BTN_BG   0x2126   // dark blue-grey — button fill
#define SW_SLOT_BG  0x0841   // near-black — empty VU bar slots
#define SW_GRN_DIM  0x0180   // very dim green — inactive RSSI bars
#define SW_DGREY    0x2945   // dark grey — inactive volume dots

// ─── Layout Y positions ───────────────────────────────────────────────────────
#define SW_LINE1     24   // amber divider — bottom of header
#define SW_DIAL_LN   44   // horizontal tuning-dial line
#define SW_LINE2     66   // amber divider — bottom of dial zone
#define SW_NAME_Y    72   // top of station-name text (font 4 = 26 px high)
#define SW_LINE3    108   // amber divider — bottom of name zone
#define SW_VU_BOT   162   // VU bars bottom Y (bars grow upward from here)
#define SW_INFO_Y   166   // info-row text baseline
#define SW_LINE4    178   // amber divider — bottom of VU + info
#define SW_TCK_Y    182   // ticker sprite push Y
#define SW_LINE5    201   // amber divider — bottom of ticker
#define SW_BTN_Y    206   // button rect top Y

// ─── Tuning-dial X geometry ───────────────────────────────────────────────────
#define DIAL_X1      20   // left end of dial line
#define DIAL_X2     300   // right end  (spacing = 280/7 = 40 px per station)

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
    sprite2.setTextColor(SW_AMBER, TFT_BLACK);

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
    M5.Lcd.startWrite();
    M5.Lcd.fillScreen(TFT_BLACK);

    // ── Header bar ──────────────────────────────────────────────────────────
    M5.Lcd.fillRect(0, 0, SCREEN_W, SW_LINE1, SW_HDR_BG);
    M5.Lcd.setTextFont(2);
    M5.Lcd.setTextColor(SW_AMBER, SW_HDR_BG);
    M5.Lcd.drawString("M5 SHORTWAVE", 6, 4);

    // ── Five amber dividers ──────────────────────────────────────────────────
    const int divs[] = { SW_LINE1, SW_LINE2, SW_LINE3, SW_LINE4, SW_LINE5 };
    for (int i = 0; i < 5; i++)
        M5.Lcd.drawFastHLine(0, divs[i], SCREEN_W, SW_AMBER);

    // ── Tuning dial: line + tick marks + station numbers ─────────────────────
    M5.Lcd.drawFastHLine(DIAL_X1, SW_DIAL_LN, DIAL_X2 - DIAL_X1 + 1, SW_AMBER_D);
    int dialSpacing = (DIAL_X2 - DIAL_X1) / (ns - 1);
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextColor(SW_AMBER_D, TFT_BLACK);
    for (int i = 0; i < ns; i++) {
        int tx = DIAL_X1 + i * dialSpacing;
        M5.Lcd.drawFastVLine(tx, 32, SW_DIAL_LN - 32, SW_AMBER_D);
        char lbl[3];
        snprintf(lbl, sizeof(lbl), "%d", i + 1);
        M5.Lcd.drawString(lbl, tx - 3, 25);
    }

    // ── VU empty bar slots (14 columns × 4 steps) ───────────────────────────
    for (int i = 0; i < 14; i++) {
        int bx = 10 + i * 21;
        for (int j = 0; j < 4; j++)
            M5.Lcd.fillRect(bx, SW_VU_BOT - 10 - j * 13, 19, 10, SW_SLOT_BG);
    }

    // ── Button row ───────────────────────────────────────────────────────────
    const char *btnLbls[] = { "SET", "STA", "VOL" };
    const int   btnCx[]   = {  53,   160,   267 };
    M5.Lcd.setTextFont(2);
    for (int i = 0; i < 3; i++) {
        int bx = btnCx[i] - 30;
        M5.Lcd.fillRoundRect(bx, SW_BTN_Y, 60, 24, 5, SW_BTN_BG);
        M5.Lcd.drawRoundRect(bx, SW_BTN_Y, 60, 24, 5, SW_AMBER_D);
        M5.Lcd.setTextColor(SW_AMBER, SW_BTN_BG);
        M5.Lcd.drawCentreString(btnLbls[i], btnCx[i], SW_BTN_Y + 4, 2);
    }

    // ── Credit ───────────────────────────────────────────────────────────────
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextColor(SW_AMBER_D, TFT_BLACK);
    M5.Lcd.drawCentreString("VOLOS / COPILOT SKIN 2026", 160, 231, 1);

    M5.Lcd.endWrite();
}

// ---------------------------------------------------------------------------
// drawDynamic — repaint only the regions that change each cycle.
// Never fills the full screen, so there is no visible flicker.
void drawDynamic() {
    M5.Lcd.startWrite();

    // ── Header: erase dynamic zone ─────────────────────────────────────────
    M5.Lcd.fillRect(120, 2, 198, 20, SW_HDR_BG);

    // WiFi connection dot
    M5.Lcd.fillCircle(127, 12, 4, connected ? TFT_GREEN : TFT_RED);

    // RSSI signal bars (4 bars, heights 4/7/10/13 px, growing upward)
    int sigBars = 0;
    if (connected) {
        if      (rssi > -55) sigBars = 4;
        else if (rssi > -65) sigBars = 3;
        else if (rssi > -75) sigBars = 2;
        else                 sigBars = 1;
    }
    for (int i = 0; i < 4; i++) {
        int bh = 4 + i * 3;
        int bx = 135 + i * 8;
        M5.Lcd.fillRect(bx, 22 - bh, 5, bh, (i < sigBars) ? TFT_GREEN : SW_GRN_DIM);
    }

    // Station counter "STA X/8"
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextColor(SW_AMBER_D, SW_HDR_BG);
    char staCur[8];
    snprintf(staCur, sizeof(staCur), "STA%d/%d", chosen + 1, ns);
    M5.Lcd.drawString(staCur, 166, 8);

    // Voltage
    M5.Lcd.setTextColor(SW_AMBER, SW_HDR_BG);
    M5.Lcd.drawString(String(voltage, 2) + "V", 218, 8);

    // Battery icon
    M5.Lcd.drawRect(253, 6, 20, 12, SW_AMBER);
    M5.Lcd.fillRect(255, 8, 16, 8, TFT_BLACK);
    M5.Lcd.fillRect(255, 8, constrain(batLevel, 0, 13), 8,
                    batLevel > 4 ? TFT_GREEN : TFT_RED);
    M5.Lcd.fillRect(273, 9, 2, 4, SW_AMBER);   // terminal nub

    // ── Tuning needle ──────────────────────────────────────────────────────
    M5.Lcd.fillRect(0, SW_DIAL_LN + 1, SCREEN_W, 14, TFT_BLACK);
    int dialSpacing = (DIAL_X2 - DIAL_X1) / (ns - 1);
    int nx = DIAL_X1 + chosen * dialSpacing;
    M5.Lcd.fillTriangle(nx,     SW_DIAL_LN + 1,
                        nx - 6, SW_DIAL_LN + 13,
                        nx + 6, SW_DIAL_LN + 13, TFT_GREEN);

    // ── Station name (font 4 = 26 px, centered) ────────────────────────────
    M5.Lcd.fillRect(0, SW_LINE2 + 1, SCREEN_W, SW_LINE3 - SW_LINE2 - 1, TFT_BLACK);
    M5.Lcd.setTextFont(4);
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Lcd.drawCentreString(stationNames[chosen], 160, SW_NAME_Y, 4);

    // ── VU bars (14 columns × 4 steps, colour-graded bottom→top) ──────────
    static const uint16_t vuOn[4] = { 0x07E0, 0x47E0, SW_AMBER, TFT_ORANGE };
    for (int i = 0; i < 14; i++) {
        g[i] = connected ? random(1, 5) : 0;
        int bx = 10 + i * 21;
        for (int j = 0; j < 4; j++) {
            int by = SW_VU_BOT - 10 - j * 13;
            M5.Lcd.fillRect(bx, by, 19, 10, j < g[i] ? vuOn[j] : SW_SLOT_BG);
        }
    }

    // ── Info row: bitrate + volume dots ────────────────────────────────────
    M5.Lcd.fillRect(0, SW_INFO_Y - 1, SCREEN_W, 12, TFT_BLACK);
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextColor(SW_AMBER, TFT_BLACK);
    M5.Lcd.drawString("BR:" + String(bitrate) + "k", 6, SW_INFO_Y);
    M5.Lcd.drawString("VOL:", 200, SW_INFO_Y);
    for (int i = 0; i < 5; i++)
        M5.Lcd.fillRect(227 + i * 11, SW_INFO_Y, 9, 7,
                        i < volume ? SW_AMBER : SW_DGREY);

    M5.Lcd.endWrite();

    canDraw = false;
}

void draw3() {
    songposition--;
    if (songposition < -310) songposition = 310;
    sprite2.fillSprite(TFT_BLACK);
    sprite2.drawString(">> " + songPlaying, songposition, 0);
    sprite2.pushSprite(5, SW_TCK_Y);
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
