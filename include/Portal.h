#pragma once

#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Persisted settings (only WiFi credentials needed for the radio)
// ---------------------------------------------------------------------------
static char rd_wifi_ssid[64] = "";
static char rd_wifi_pass[64] = "";
static bool rd_has_settings  = false;

// ---------------------------------------------------------------------------
// Portal state
// ---------------------------------------------------------------------------
static WebServer *portalServer = nullptr;
static DNSServer *portalDNS    = nullptr;
static bool       portalDone   = false;

// ---------------------------------------------------------------------------
// NVS load / save
// ---------------------------------------------------------------------------
static void rdLoadSettings() {
    Preferences prefs;
    prefs.begin("m5radio", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();
    ssid.toCharArray(rd_wifi_ssid, sizeof(rd_wifi_ssid));
    pass.toCharArray(rd_wifi_pass, sizeof(rd_wifi_pass));
    rd_has_settings = (ssid.length() > 0);
}

static void rdSaveSettings(const char* ssid, const char* pass) {
    Preferences prefs;
    prefs.begin("m5radio", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    strncpy(rd_wifi_ssid, ssid, sizeof(rd_wifi_ssid) - 1);
    strncpy(rd_wifi_pass, pass, sizeof(rd_wifi_pass) - 1);
    rd_has_settings = true;
}

// ---------------------------------------------------------------------------
// On-screen setup instructions
// ---------------------------------------------------------------------------
static void rdShowPortalScreen() {
    M5.Lcd.fillScreen(TFT_BLACK);

    M5.Lcd.setTextColor(TFT_GREEN);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(20, 8);
    M5.Lcd.print("M5 Radio Setup");

    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(40, 34);
    M5.Lcd.print("WiFi Internet Radio by Volos");

    M5.Lcd.setTextColor(TFT_YELLOW);
    M5.Lcd.setCursor(4, 58);
    M5.Lcd.print("1. Connect your phone/PC to:");
    M5.Lcd.setTextColor(TFT_GREEN);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(20, 70);
    M5.Lcd.print("M5Radio_Setup");

    M5.Lcd.setTextColor(TFT_YELLOW);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(4, 96);
    M5.Lcd.print("2. Open browser to:");
    M5.Lcd.setTextColor(TFT_GREEN);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(50, 108);
    M5.Lcd.print("192.168.4.1");

    M5.Lcd.setTextColor(TFT_YELLOW);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(4, 134);
    M5.Lcd.print("3. Enter WiFi credentials & Save.");

    if (rd_has_settings) {
        M5.Lcd.setTextColor(TFT_CYAN);
        M5.Lcd.setCursor(4, 158);
        M5.Lcd.print("Saved settings found.");
        M5.Lcd.setCursor(4, 170);
        M5.Lcd.print("Tap 'No Changes' to keep them.");
    }
}

// ---------------------------------------------------------------------------
// Web handlers
// ---------------------------------------------------------------------------
static void rdHandleRoot() {
    String html =
        "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>M5 Radio Setup</title>"
        "<style>"
        "body{background:#0d1a0d;color:#00cc44;font-family:Arial,sans-serif;"
             "text-align:center;padding:20px;max-width:480px;margin:auto;}"
        "h1{color:#00ff55;font-size:1.6em;margin-bottom:4px;}"
        "p{color:#226633;font-size:0.9em;}"
        "label{display:block;text-align:left;margin:14px 0 4px;color:#44aa66;"
              "font-weight:bold;}"
        "input[type=text],input[type=password]{width:100%;box-sizing:border-box;"
              "background:#0a1a0a;color:#66ff88;border:2px solid #224422;"
              "border-radius:6px;padding:10px;font-size:1em;}"
        ".btn{display:block;width:100%;padding:14px;margin:10px 0;font-size:1.05em;"
             "border-radius:8px;border:none;cursor:pointer;font-weight:bold;}"
        ".btn-save{background:#112211;color:#00ff55;border:2px solid #224422;}"
        ".btn-save:hover{background:#223322;}"
        ".btn-skip{background:#111;color:#555;border:2px solid #333;}"
        ".btn-skip:hover{background:#222;color:#888;}"
        ".note{color:#224433;font-size:0.82em;margin-top:16px;}"
        "hr{border:1px solid #112211;margin:20px 0;}"
        "</style></head><body>"
        "<h1>&#127925; M5 Radio</h1>"
        "<p>WiFi Internet Radio &mdash; Volos Projects</p>"
        "<form method='post' action='/save'>"
        "<label>WiFi Network (SSID):</label>"
        "<input type='text' name='ssid' value='";
    html += String(rd_wifi_ssid);
    html +=
        "' placeholder='Your 2.4 GHz WiFi name' maxlength='63' required>"
        "<label>WiFi Password:</label>"
        "<input type='password' name='pass' value='";
    html += String(rd_wifi_pass);
    html +=
        "' placeholder='Leave blank if open network' maxlength='63'>"
        "<br>"
        "<button class='btn btn-save' type='submit'>&#128190; Save &amp; Connect</button>"
        "</form>";

    if (rd_has_settings) {
        html +=
            "<hr>"
            "<form method='post' action='/nochange'>"
            "<button class='btn btn-skip' type='submit'>"
            "&#10006; No Changes &mdash; Use Current Settings"
            "</button></form>";
    }

    html +=
        "<p class='note'>&#9888; ESP32 supports 2.4 GHz WiFi only.</p>"
        "</body></html>";

    portalServer->send(200, "text/html", html);
}

static void rdHandleSave() {
    String ssid = portalServer->hasArg("ssid") ? portalServer->arg("ssid") : "";
    String pass = portalServer->hasArg("pass") ? portalServer->arg("pass") : "";

    if (ssid.length() == 0) {
        portalServer->send(400, "text/html",
            "<html><body style='background:#0d1a0d;color:#ff5555;font-family:Arial;"
            "text-align:center;padding:40px'>"
            "<h2>&#10060; SSID cannot be empty!</h2>"
            "<a href='/' style='color:#00ff55'>&#8592; Go Back</a></body></html>");
        return;
    }

    rdSaveSettings(ssid.c_str(), pass.c_str());

    portalServer->send(200, "text/html",
        "<html><head><meta charset='UTF-8'>"
        "<style>body{background:#0d1a0d;color:#00ff55;font-family:Arial;"
        "text-align:center;padding:40px;}</style></head><body>"
        "<h2>&#9989; Settings Saved!</h2>"
        "<p>Connecting to <b>" + ssid + "</b>...</p>"
        "<p>Close this page and disconnect from <b>M5Radio_Setup</b>.</p>"
        "</body></html>");
    delay(1500);
    portalDone = true;
}

static void rdHandleNoChange() {
    portalServer->send(200, "text/html",
        "<html><head><meta charset='UTF-8'>"
        "<style>body{background:#0d1a0d;color:#00ff55;font-family:Arial;"
        "text-align:center;padding:40px;}</style></head><body>"
        "<h2>&#128077; No Changes</h2>"
        "<p>Using saved settings. Radio connecting now.</p>"
        "<p>Disconnect from <b>M5Radio_Setup</b>.</p>"
        "</body></html>");
    delay(1500);
    portalDone = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
static void rdInitPortal() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("M5Radio_Setup", "");
    delay(500);

    portalDNS    = new DNSServer();
    portalServer = new WebServer(80);

    portalDNS->start(53, "*", WiFi.softAPIP());
    portalServer->on("/",         rdHandleRoot);
    portalServer->on("/save",     HTTP_POST, rdHandleSave);
    portalServer->on("/nochange", HTTP_POST, rdHandleNoChange);
    portalServer->onNotFound(rdHandleRoot);
    portalServer->begin();

    portalDone = false;
    rdShowPortalScreen();

    Serial.printf("[Portal] AP up — connect to M5Radio_Setup, open %s\n",
                  WiFi.softAPIP().toString().c_str());
}

static void rdRunPortal()   { portalDNS->processNextRequest(); portalServer->handleClient(); }

static void rdClosePortal() {
    portalServer->stop();
    portalDNS->stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(300);
    delete portalServer; portalServer = nullptr;
    delete portalDNS;    portalDNS    = nullptr;
}
