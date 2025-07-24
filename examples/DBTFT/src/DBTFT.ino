#include <Arduino.h>
#include <DBAPI.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TimeLib.h>

//#define BOARD_2432S028R //ESP32 + ILI9341 2.8" TFT
//#define USE_ST7798      //2432S028R with ST7798
#define AUTOCONFIG
#define ENABLEOTA
//#define DEBUG_BRIGHTNESS

#ifdef USE_ST7798
#include <Adafruit_ST7789.h>
#else
#include <Adafruit_ILI9341.h>
#endif

#ifdef AUTOCONFIG
	const char* apSSID = "DBTafel";
	const char* apPassword = "nichtPuenktlich";
	#include <LittleFS.h>
	#include <WiFiManager.h>
#else
	const char* ssid = "SSID";
	const char* password = "password";
#endif
#ifdef ENABLEOTA
	const char* otaPassword = "iliketrains";
	#include <ArduinoOTA.h>
#endif
char fromStationName[40] = "Hannnover Hbf";
uint16_t filter = PROD_RE | PROD_S;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

#define BACKGROUND_COLOR 0x001F // Blue
#define FOREGROUND_COLOR 0xFFFF // White
#define HIGHLIGHT_COLOR  0xFFE0 // Yellow

// Optional view with large destinations, does interfere with platform when station names are longer
#define WIDE_MODE
#define SCROLL_CHARS     13
#define SCROLL_STALL      5
#define SCROLL_INTERVAL 400
//#define SCROLLING_ENABLED
//#define TEXT_FILL

DBAPI db;
DBstation* fromStation;
DBdeparr* da = NULL;
DBdeparr* departure = NULL;
uint32_t nextCheck;
uint32_t nextTime;
uint32_t nextScroll;
uint32_t nextBrightness;
time_t old_time;
bool updateInhibit = false;
uint16_t currentBrightness = 0;
uint8_t  rotation = 1;

#ifdef BOARD_2432S028R
	#define HSPI_SCLK 14
	#define HSPI_MISO 12
	#define HSPI_MOSI 13
	#define TFT_DC  2
	#define TFT_CS 15
	#define TFT_BL 21
	#define LDR_IN 34
	// GT36516 LDR -> GND, 1M Pullup, 1M Pulldown
	// 1V dark
	// 0V bright
#else
	#define HSPI_SCLK 14
	#define HSPI_MISO 12
	#define HSPI_MOSI 13
	#define TFT_DC 16
	#define TFT_CS 15
	#define TFT_BL  5
	#define LDR_IN A0 // optional LDR
#endif
#define BUT_IN 0
#define MIN_BRIGHTNESS   2
#define MAX_BRIGHTNESS 255 // Nothing above noteworthy
#ifdef ESP32
	SPIClass* hspi = new SPIClass(HSPI);
#ifdef USE_ST7798
	Adafruit_ST7789 tft = Adafruit_ST7789(hspi, TFT_CS, TFT_DC, -1);
#else
	Adafruit_ILI9341 tft = Adafruit_ILI9341(hspi, TFT_DC, TFT_CS);
#endif
#else
#ifdef USE_ST7798
	Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, -1);
#else
	Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC);
#endif
#endif

#ifdef AUTOCONFIG
#endif

time_t getNtpTime() {
	return timeClient.getEpochTime();
}

void drawStaticContent() {
	tft.fillScreen(BACKGROUND_COLOR);
	tft.setTextColor(FOREGROUND_COLOR);
	tft.setTextSize(2);
	//tft.setCursor((tft.width() - (strlen(fromStationName) * 6 - 1) * 2) / 2, 2);
	tft.setCursor(11 * 6 + 6, 2);
	tft.print(fromStationName);
	tft.fillRect(0, 18, tft.width(), 18, HIGHLIGHT_COLOR);
	tft.setTextColor(BACKGROUND_COLOR);
	tft.setCursor(2, 20);
	tft.print("Zeit");
#ifdef WIDE_MODE
	tft.setCursor(11 * 6 + 6, 20);
#else // WIDE_MODE
	tft.setCursor(9 * 6 + 6, 20);
#endif // WIDE_MODE
	tft.print("Nach");
	tft.setCursor(tft.width() - 7 * 6 * 2, 20);
	tft.print("Gleis");
}

#ifdef ENABLEOTA
void updateStarted() {
	updateInhibit = true;
	Serial.println("Update started...");
	tft.fillScreen(BACKGROUND_COLOR);
	tft.setTextColor(FOREGROUND_COLOR);
	tft.setCursor(0, 0);
	tft.setTextSize(2);
	tft.println("Update...");
}
#endif

#ifdef AUTOCONFIG
WiFiManager wifiManager;
WiFiManagerParameter stationNameParameter("stationName", "Stationsname", fromStationName, sizeof(fromStationName));
WiFiManagerParameter rotationParameter("rotation", "Rotation (1 oder 3)", String(rotation).c_str(), 1);
WiFiManagerParameter filterICEParameter("filterICE", "Hochgeschwindigkeitsz&uuml;ge (1 zum Aktivieren)", String((filter & PROD_ICE) > 0).c_str(), 1);
WiFiManagerParameter filterICECParameter("filterICEC", "Intercity- und Eurocityz&uuml;ge 1 zum Aktivieren)", String((filter & PROD_IC_EC) > 0).c_str(), 1);
WiFiManagerParameter filterIRParameter("filterIR", "Interregio- und Schnellz&uuml;ge (1 zum Aktivieren)", String((filter & PROD_IR) > 0).c_str(), 1);
WiFiManagerParameter filterREParameter("filterRE", "Nahverkehr, sonsitge Z&uuml;ge (1 zum Aktivieren)", String((filter & PROD_RE) > 0).c_str(), 1);
WiFiManagerParameter filterSParameter("filterS", "S-Bahnen (1 zum Aktivieren)", String((filter & PROD_S) > 0).c_str(), 1);
WiFiManagerParameter filterBUSParameter("filterBUS", "Busse (1 zum Aktivieren)", String((filter & PROD_BUS) > 0).c_str(), 1);
WiFiManagerParameter filterSHIPParameter("filterSHIP", "Schiffe (1 zum Aktivieren)", String((filter & PROD_SHIP) > 0).c_str(), 1);
WiFiManagerParameter filterUParameter("filterU", "U-Bahnen (1 zum Aktieren)", String((filter & PROD_U) > 0).c_str(), 1);
WiFiManagerParameter filterSTBParameter("filterSTB", "Stra&szlig;enbahnen (1 zum Aktivieren)", String((filter & PROD_STB) > 0).c_str(), 1);
WiFiManagerParameter filterASTParameter("filterAST", "Anrufpflichtige Verkehre (1 zum Aktivieren)", String((filter & PROD_AST) > 0).c_str(), 1);
bool shouldSaveConfig = false;

void configModeCallback (WiFiManager *myWiFiManager) {
	tft.fillScreen(BACKGROUND_COLOR);
	tft.setTextColor(FOREGROUND_COLOR);
	tft.setTextSize(2);
	tft.setCursor(0, 0);
	tft.println("Zur Konfiguration mit\nAccess-Point verbinden:");
	tft.println(apSSID);
	tft.println(apPassword);
	if (WiFi.isConnected()) {
		tft.println("Oder im lokalen WLAN:");
		tft.println(WiFi.localIP());
	}
}

void failedConfig() {
	tft.fillScreen(0xF800); // red
	tft.setTextColor(FOREGROUND_COLOR);
	tft.setTextSize(2);
	tft.setCursor(0, 0);
	tft.println("WLAN Verbindung fehl\n-geschlagen");
	tft.println("Konfiguration nicht\nerfolgt");
	tft.print("Starte neu...");
	delay(5000);
	ESP.restart();
	delay(5000);
}

void saveConfigCallback () {
	shouldSaveConfig = true;
	Serial.println("New data to save");
}

void readParams() {
	if (LittleFS.exists("/station")) {
		File f = LittleFS.open("/station", "r");
		if (f) {
			uint8_t i = 0;
			while (f.available() && i < sizeof(fromStationName)) {
				fromStationName[i++] = f.read();
			}
			if (i < sizeof(fromStationName))
			fromStationName[i] = 0;
		} else {
			Serial.println("File not open");
		}
	} else {
		Serial.println("File not found");
	}
	if (LittleFS.exists("/rotation")) {
		File f = LittleFS.open("/rotation", "r");
		if (f) {
			uint8_t r = f.read() - '0';
			if (r == 1 || r == 3) {
				rotation = r;
			}
		} else {
			Serial.println("File not open");
		}
	} else {
		Serial.println("File not found");
	}
	if (LittleFS.exists("/filter")) {
		File f = LittleFS.open("/filter", "r");
		if (f) {
			String s = f.readString();
			long val = s.toInt();
			if (val > 0 && val <= 1023) {
				filter = val;
			} else {
				Serial.print("Invalid filter value: ");
				Serial.print(s);
				Serial.print(" / ");
				Serial.println(val);
			}
		} else {
			Serial.println("File not open");
		}
	} else {
		Serial.println("File not found");
	}
}

void checkConfigRequest() {
	if (!digitalRead(BUT_IN)) {
		if (!wifiManager.startConfigPortal(apSSID, apPassword)) {
			// exited
		}
		afterConfigCallback();
		drawStaticContent();
		updateInhibit = false;
		nextCheck = 0;
	}
}

void afterConfigCallback() {
	if (shouldSaveConfig) {
		{
			strncpy(fromStationName, stationNameParameter.getValue(), sizeof(fromStationName));
			Serial.print("Stationsname gesetzt: ");
			Serial.println(fromStationName);
			File f = LittleFS.open("/station", "w");
			f.print(fromStationName);
			f.close();
		}
		uint8_t r = rotationParameter.getValue()[0];
		if (r == '1' || r == '3') {
			rotation = r - '0';
			File f = LittleFS.open("/rotation", "w");
			f.write(r);
			f.close();
			tft.setRotation(rotation);
			Serial.print("Rotation gesetzt: ");
			Serial.println(rotation);
		}
		uint16_t val =
			(filterICEParameter.getValue()[0]  == '1') * PROD_ICE |
			(filterICECParameter.getValue()[0] == '1') * PROD_IC_EC |
			(filterIRParameter.getValue()[0]   == '1') * PROD_IR |
			(filterREParameter.getValue()[0]   == '1') * PROD_RE |
			(filterSParameter.getValue()[0]    == '1') * PROD_S |
			(filterBUSParameter.getValue()[0]  == '1') * PROD_BUS |
			(filterSHIPParameter.getValue()[0] == '1') * PROD_SHIP |
			(filterUParameter.getValue()[0]    == '1') * PROD_U |
			(filterSTBParameter.getValue()[0]  == '1') * PROD_STB |
			(filterASTParameter.getValue()[0]  == '1') * PROD_AST;
		if (val > 0) {
			File f = LittleFS.open("/filter", "w");
			f.print(val);
			f.close();
			filter = val;
			Serial.print("Filter gesetzt: ");
			Serial.println(filter);
		} else {
			Serial.print("Filter setzen fehlgeschlagen: ");
			Serial.println(val);
		}
	} else {
		Serial.println("Nicht speichern.");
	}
}
#endif

void setup() {
	Serial.begin(115200);
#ifdef ESP32
	//hspi->begin();
	hspi->begin(HSPI_SCLK, HSPI_MISO, HSPI_MOSI, TFT_CS);
#endif

#ifdef USE_ST7798
	tft.init(240, 320, SPI_MODE0);
	tft.invertDisplay(0);
#else
	tft.begin();
#endif
#ifdef BOARD_2432S028R
	pinMode(4, OUTPUT); //LED RED
	digitalWrite(4, 1);
	pinMode(16, OUTPUT); //LED GREEN
	digitalWrite(16, 1);
	pinMode(17, OUTPUT); //LED BLUE
	digitalWrite(17, 1);
#endif
	WiFi.mode(WIFI_STA);
	pinMode(BUT_IN, INPUT_PULLUP);
#ifndef AUTOCONFIG
	WiFi.mode(WIFI_STA);
	WiFi.begin(ssid, password);
#else
#ifdef ESP32
	if (!LittleFS.begin(1)) {
#else
	if (!LittleFS.begin()) {
#endif
		Serial.println("LittleFS mount failed");
	} else {
		readParams();
	}
	wifiManager.setAPCallback(configModeCallback);
	stationNameParameter.setValue(fromStationName, sizeof(fromStationName));
	wifiManager.addParameter(&stationNameParameter);
	rotationParameter.setValue(String(rotation).c_str(), 1);
	wifiManager.addParameter(&rotationParameter);
	filterICEParameter.setValue(String((filter & PROD_ICE) > 0).c_str(), 1);
	wifiManager.addParameter(&filterICEParameter);
	filterICECParameter.setValue(String((filter & PROD_IC_EC) > 0).c_str(), 1);
	wifiManager.addParameter(&filterICECParameter);
	filterIRParameter.setValue(String((filter & PROD_IR) > 0).c_str(), 1);
	wifiManager.addParameter(&filterIRParameter);
	filterREParameter.setValue(String((filter & PROD_RE) > 0).c_str(), 1);
	wifiManager.addParameter(&filterREParameter);
	filterSParameter.setValue(String((filter & PROD_S) > 0).c_str(), 1);
	wifiManager.addParameter(&filterSParameter);
	filterBUSParameter.setValue(String((filter & PROD_BUS) > 0).c_str(), 1);
	wifiManager.addParameter(&filterBUSParameter);
	filterSHIPParameter.setValue(String((filter & PROD_SHIP) > 0).c_str(), 1);
	wifiManager.addParameter(&filterSHIPParameter);
	filterUParameter.setValue(String((filter & PROD_U) > 0).c_str(), 1);
	wifiManager.addParameter(&filterUParameter);
	filterSTBParameter.setValue(String((filter & PROD_STB) > 0).c_str(), 1);
	wifiManager.addParameter(&filterSTBParameter);
	filterASTParameter.setValue(String((filter & PROD_AST) > 0).c_str(), 1);
	wifiManager.addParameter(&filterASTParameter);
	wifiManager.setSaveConfigCallback(saveConfigCallback);
	wifiManager.setConnectTimeout(30);
	wifiManager.setConfigPortalTimeout(120);
#endif
	pinMode(TFT_BL, OUTPUT);
	digitalWrite(TFT_BL, HIGH);
	tft.fillScreen(BACKGROUND_COLOR);
	tft.setTextColor(FOREGROUND_COLOR);
	tft.setRotation(rotation);
	tft.setTextSize(2);
	tft.print("Verbinde...");
 	Serial.print("Verbinde...");
#ifdef AUTOCONFIG
	if (!wifiManager.autoConnect(apSSID, apPassword)) {
		afterConfigCallback(); // try to save either way 
		failedConfig();
	}
	afterConfigCallback();
#else
	while (WiFi.status() != WL_CONNECTED) {
		tft.write('.');
    	Serial.write('.');
		delay(500);
	}
#endif
#ifdef ENABLEOTA
	ArduinoOTA.onStart(updateStarted);
	ArduinoOTA.onEnd([]() {
		updateInhibit = false;
		tft.println("Fertig, starte neu.");
	});
	ArduinoOTA.onError([](ota_error_t e) {
		updateInhibit = false;
		tft.println("Fehler.");
		delay(5000);
		drawStaticContent();
	});
	ArduinoOTA.setPassword(otaPassword);
	ArduinoOTA.begin();
#endif
#ifdef ESP32
	analogSetAttenuation(ADC_2_5db); // ESP8266 equivalent 1.1V max
#endif
	db.setAGFXOutput(true);
	Serial.println();
	fromStation = db.getStation(fromStationName);
	yield();
	timeClient.begin();
	timeClient.setTimeOffset(3600); // CET
	adjustTime(3600);
	setSyncProvider(getNtpTime);
	drawStaticContent();
}

uint32_t scroll;
time_t tdst;
void loop() {
	if (nextTime < millis() && !updateInhibit) {
		timeClient.update();
		time_t tnow = timeClient.getEpochTime();
		tdst = dst(tnow);
		if (old_time / 60 != tdst / 60) {
			tft.setTextColor(FOREGROUND_COLOR);
			tft.setTextSize(2);
			tft.setCursor(2, 2);
			tft.fillRect(2, 2, 5 * 6 * 2 , 16, BACKGROUND_COLOR);
			if (hour(tdst) < 10) tft.write('0');
			tft.print(hour(tdst));
			tft.write(':');
			if (minute(tdst) < 10) tft.write('0');
			tft.print(minute(tdst));
			old_time = tdst;
		}
		nextTime = millis() + 1000;
	}
	if (nextCheck < millis() && !updateInhibit) {
		if (WiFi.status() != WL_CONNECTED) {
			Serial.println("Disconnected");
		}
		Serial.println("Reload");
		da = db.getDepartures(fromStation->stationId, NULL, tdst, 11, 3, filter);
		Serial.println();
		departure = da;
		uint16_t pos = 21;
		char buf[10];
	    while (departure != NULL) {
			pos += 18;
			if (pos + 16 > tft.height()) break;
#ifdef WIDE_MODE
			tft.fillRect(0, pos - 1, 11 * 6 + 4 , 17, FOREGROUND_COLOR);
#else // WIDE_MODE
			tft.fillRect(0, pos - 1, 9 * 6 + 4 , 17, FOREGROUND_COLOR);
#endif // WIDE_MODE
			tft.setTextColor(BACKGROUND_COLOR);
			tft.setTextSize(1);
			tft.setCursor(2, pos);
			snprintf(buf, sizeof(buf), "%02d:%02d", hour(departure->time), minute(departure->time));
			tft.print(buf);
#ifdef WIDE_MODE
			if (!departure->cancelled && departure->delay) {
				tft.print(" +");
				tft.print(departure->delay);
			}
#endif // WIDE_MODE
			tft.setCursor(2, pos + 8);
			tft.print(departure->product);
			if (strcmp("", departure->textline)) {
				tft.write(' ');
				tft.print(departure->textline);
			}
			tft.setTextColor(FOREGROUND_COLOR);
#ifdef WIDE_MODE
			tft.fillRect(11 * 6 + 4, pos - 1, tft.width(), 17, BACKGROUND_COLOR);
			printScroll(departure->target, 11 * 6 + 6, pos, true, departure->cancelled);
#else // WIDE_MODE
			tft.fillRect(9 * 6 + 4, pos - 1, tft.width(), 17, BACKGROUND_COLOR);
			tft.setCursor(9 * 6 + 6, pos);
			tft.setTextSize(1);
			tft.print(departure->target);
#endif // WIDE_MODE

#ifndef WIDE_MODE
			tft.setTextColor(HIGHLIGHT_COLOR);
			tft.setCursor(9 * 6 + 6, pos + 8);
			if (departure->cancelled) {
				tft.print("Fahrt f\x84llt aus");
				tft.drawFastHLine(9 * 6 + 6, pos + 3, strlen(departure->target) * 6 - 1, FOREGROUND_COLOR);
			} else if (departure->delay) {
				tft.print("ca. ");
				tft.print(departure->delay);
				tft.print(" Minuten sp\x84ter");
			}
#endif
			tft.setTextColor(FOREGROUND_COLOR);
			tft.setTextSize(2);
			tft.setCursor(tft.width() - 7 * 6 * 2, pos);
			if (strcmp("", departure->newPlatform) && strcmp(departure->platform, departure->newPlatform)) {
				tft.setTextColor(HIGHLIGHT_COLOR);
				//tft.print("->");
				tft.print(departure->newPlatform);
			} else {
				tft.print(departure->platform);
			}
			departure = departure->next;
		}
		// clear empty spots (not enough departures)
		while (pos + 16 + 18 <= tft.height()) {
			pos += 18;
			tft.fillRect(0, pos - 1, tft.width(), 17, BACKGROUND_COLOR);
		}
		nextCheck = millis() + 50000;
	}
#ifdef WIDE_MODE
	if (nextScroll < millis() && !updateInhibit) {
		departure = da;
		uint16_t pos = 21;
	    while (departure != NULL) {
			pos += 18;
			printScroll(departure->target, 11 * 6 + 6, pos, false, departure->cancelled);
			departure = departure->next;
		}
		scroll++;
		nextScroll = millis() + SCROLL_INTERVAL;
	}
#endif
#ifdef ENABLEOTA
	ArduinoOTA.handle();
#endif
#ifdef AUTOCONFIG
	checkConfigRequest();
#endif
#ifdef LDR_IN
	if (nextBrightness < millis()) {
		uint16_t b = analogRead(LDR_IN);
#ifdef ESP32
		b >>= 2;
#endif
		if (b < currentBrightness) {
			currentBrightness--;
		} else if (b > currentBrightness) {
			currentBrightness++;
		}
		uint16_t brightness = map(currentBrightness, 1023, 0, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
		analogWrite(TFT_BL, brightness);
#ifdef DEBUG_BRIGHTNESS
		Serial.print("B:\t");
		Serial.print(b);
		Serial.print("\tCB:\t");
		Serial.print(currentBrightness);
		Serial.print("\tL:\t");
		Serial.println(brightness);
#endif
		nextBrightness = millis() + 10;
	}
#endif
}

void printScroll(String text, uint16_t x, uint16_t y, bool force, bool cancelled) {
#ifdef TEXT_FILL
	tft.setTextColor(FOREGROUND_COLOR, BACKGROUND_COLOR);
#else // TEXT_FILL
	tft.setTextColor(FOREGROUND_COLOR);
#endif // TEXT_FILL
	tft.setTextSize(2);
	if (text.length() > SCROLL_CHARS || force) {
#ifdef SCROLLING_ENABLED
		uint32_t p = scroll;
		int16_t ts = text.length() - SCROLL_CHARS;
		if (ts < 0) {
			ts = 0;
		}
		uint32_t to_scroll = ts;
		p %= to_scroll * 2 + SCROLL_STALL * 2;
		if (p <= to_scroll) {
			if (!force && p == 0) return; // do not update 0 position
		} else if (p <= to_scroll + SCROLL_STALL) {
			p = to_scroll;
			if (!force) return; // do not update on stall, if no update is forced
		} else if (p <= to_scroll * 2 + SCROLL_STALL) {
			p = SCROLL_STALL + to_scroll * 2 - p;
		} else {
			p = 0;
			if (!force) return; // do not update on stall, if no update is forced
		}
#else // SCOLLING_ENABLED
		if (!force) return;
		uint32_t p = 0;
#endif // SCROLLING_ENABLED
#ifndef TEXT_FILL
		tft.fillRect(x - 2, y - 1, SCROLL_CHARS * 6 * 2, 17, BACKGROUND_COLOR);
#endif // TEXT_FILL
		tft.setCursor(x, y);
		text = text.substring(p, p + SCROLL_CHARS);
		tft.print(text);
		if (cancelled) {
			uint8_t len = min((uint8_t) text.length(), (uint8_t) SCROLL_CHARS);
			tft.drawFastHLine(x, y + 6, (len * 6 - 1) * 2, FOREGROUND_COLOR);
			tft.drawFastHLine(x, y + 7, (len * 6 - 1) * 2, FOREGROUND_COLOR);
		}
	}
}

time_t dst(time_t t) {
	if (month(t) > 3 && month(t) < 10) { //Apr-Sep
		return t + 3600;
	} else if (month(t) == 3 && day(t) - weekday(t) >= 24) { //Date at or after last sunday in March
		if (weekday(t) == 1) { //Sunday to switch to dst
			if (hour(t) >= 2) { //Time after 2AM
				return t + 3600;
			}
		} else { //Date after last sunday in March
			return t + 3600;
		}
	} else if (month(t) == 10 && day(t) - weekday(t) < 24) { //Date before last sunday in October
		return t + 3600;
	} else if (month(t) == 10 && day(t) - weekday(t) >= 24) { //Date at or after last sunday in March
		if (weekday(t) == 1) { //Sunday to switch back from dst
			if (hour(t) < 3) { //Time before 2AM without DST (3AM DST, which doesn't exist)
				return t + 3600;
			}
		}
	}
	return t;
}
