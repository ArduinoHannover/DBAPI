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

#define COLOR_RED    0xF800
#define COLOR_WHITE  0xFFFF
#define COLOR_BLACK  0x0000
#define COLOR_BLUE   0x001F
#define COLOR_YELLOW 0xFFE0
#define COLOR_ORANGE 0xFBE0

// Optional view with large destinations, does interfere with platform when station names are longer
#ifndef ESP8266 // ESP8266 display output is too slow
#define SCROLLING_ENABLED
#endif
#define SCROLL_STALL      5
#define SCROLL_INTERVAL 400
#define TEXT_FILL

typedef struct {
	uint8_t  id;
	uint8_t  headerOffset;
	uint8_t  itemHeight;
	uint8_t  targetX;
	uint8_t  maxTextLength;
	uint8_t  maxEntries;
	uint16_t foregroundColor;
	uint16_t backgroundColor;
	uint16_t highlightColor;
	bool     canScroll;
} design_t;

enum designID {
	DESIGN_WIDE,
	DESIGN_DETAIL,
	DESIGN_LED,
	MAX_DESIGNS
};

design_t designs[] = {
	{
		DESIGN_WIDE,
		39,           //headerOffset
		18,           //itemHeight
		11 * 6 + 6,   //targetX
		13,           //maxTextLength
		11,           //maxEntries
		COLOR_WHITE,  //foreground
		COLOR_BLUE,   //background
		COLOR_YELLOW, //highlight
		true          //canScroll
	},
	{
		DESIGN_DETAIL,
		39,           //headerOffset
		18,           //itemHeight
		9 * 6 + 6,    //targetX
		30,           //maxTextLength
		11,           //maxEntries
		COLOR_WHITE,  //foreground
		COLOR_BLUE,   //background
		COLOR_YELLOW, //highlight
		false         //canScroll
	},
	{
		DESIGN_LED,
		21,           //headerOffset
		18,           //itemHeight
		4 * 12 + 6,   //targetX
		16,           //maxTextLength
		12,           //maxEntries
		COLOR_ORANGE, //foreground
		COLOR_BLACK,  //background
		COLOR_ORANGE, //highlight
		true          //canScroll
	}
};

design_t* activeDesign = &designs[0];

DBAPI db;
DBstation* fromStation;
DBdeparr* da = NULL;
DBdeparr* departure = NULL;
time_t tdst;
uint32_t nextCheck;
uint32_t nextTime;
uint32_t nextScroll;
uint32_t nextBrightness;
uint32_t scroll;
time_t old_time;
bool updateInhibit = false;
#ifdef SCROLLING_ENABLED
bool scrollingEnabled = true;
#else
bool scrollingEnabled = false;
#endif
uint16_t currentBrightness = 0;
uint8_t  rotation = 1;
uint8_t  failedConnection = 0;
uint8_t  failedUpdates = 0;
char     errorMessage[50];

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

#ifndef LDR_IN
#define LDR_IN -1
#endif

int8_t ldr_pin = LDR_IN;

#ifdef AUTOCONFIG
#endif

time_t getNtpTime() {
	return timeClient.getEpochTime();
}

void drawStaticContent() {
	tft.fillScreen(activeDesign->backgroundColor);
	tft.setTextColor(activeDesign->foregroundColor);
	tft.setTextSize(2);
	switch (activeDesign->id) {
	case DESIGN_WIDE:
	case DESIGN_DETAIL:
		tft.setCursor(11 * 6 + 6, 2);
		tft.print(fromStationName);
		tft.fillRect(0, 18, tft.width(), 18, activeDesign->highlightColor);
		tft.setTextColor(activeDesign->backgroundColor);
		tft.setCursor(2, 20);
		tft.print("Zeit");
		tft.setCursor(activeDesign->targetX, 20);
		tft.print("Nach");
		tft.setCursor(tft.width() - 7 * 6 * 2, 20);
		tft.print("Gleis");
		break;
	case DESIGN_LED:
		tft.setCursor(0, 2);
		tft.print(fromStationName);
	}
}

void drawTime() {
	tft.setTextColor(activeDesign->foregroundColor);
	tft.setTextSize(2);
	switch (activeDesign->id) {
		case DESIGN_WIDE:
		case DESIGN_DETAIL:
			tft.setCursor(2, 2);
			tft.fillRect(2, 2, 5 * 6 * 2 , 16, activeDesign->backgroundColor);
			break;
		case DESIGN_LED:
			tft.setCursor(tft.width() - 5 * 12, 2);
			tft.fillRect(tft.width() - 5 * 12, 2, 5 * 6 * 2 , 16, activeDesign->backgroundColor);
	}
	if (hour(tdst) < 10) tft.write('0');
	tft.print(hour(tdst));
	tft.write(':');
	if (minute(tdst) < 10) tft.write('0');
	tft.print(minute(tdst));
	old_time = tdst;
}

bool drawDeparture(DBdeparr* departure, uint16_t &pos) {
	Serial.println("Drawing next departure.");
	char buf[10];
	// Next drawn item would be at least partially out of frame
	if (pos + activeDesign->itemHeight - 2 > tft.height()) return false;
	switch (activeDesign->id) {
		case DESIGN_WIDE:
		case DESIGN_DETAIL: {
			switch (activeDesign->id) {
			case DESIGN_DETAIL:
				tft.fillRect(0, pos - 1, 9 * 6 + 4 , 17, activeDesign->foregroundColor);
			default:
				tft.fillRect(0, pos - 1, 11 * 6 + 4 , 17, activeDesign->foregroundColor);
			}
			tft.setTextColor(activeDesign->backgroundColor);
			tft.setTextSize(1);
			tft.setCursor(2, pos);
			snprintf(buf, sizeof(buf), "%02d:%02d", hour(departure->time), minute(departure->time));
			tft.print(buf);
			if (activeDesign->id != DESIGN_DETAIL) {
				if (!departure->cancelled && departure->delay) {
					tft.print(" +");
					tft.print(departure->delay);
				}
			}
			tft.setCursor(2, pos + 8);
			tft.print(departure->product);
			if (strcmp("", departure->textline)) {
				tft.write(' ');
				tft.print(departure->textline);
			}
			tft.setTextColor(activeDesign->foregroundColor);
			if (activeDesign->id == DESIGN_DETAIL) {
				tft.fillRect(9 * 6 + 4, pos - 1, tft.width(), 17, activeDesign->backgroundColor);
				tft.setCursor(9 * 6 + 6, pos);
				tft.setTextSize(1);
				tft.print(departure->target);

				tft.setTextColor(activeDesign->highlightColor);
				tft.setCursor(9 * 6 + 6, pos + 8);
				if (departure->cancelled) {
					tft.print("Fahrt f\x84llt aus");
					tft.drawFastHLine(9 * 6 + 6, pos + 3, strlen(departure->target) * 6 - 1, activeDesign->foregroundColor);
				} else if (departure->delay) {
					tft.print("ca. ");
					tft.print(departure->delay);
					tft.print(" Minuten sp\x84ter");
				}
			} else {
				tft.fillRect(11 * 6 + 4, pos - 1, tft.width(), 17, activeDesign->backgroundColor);
				printScroll(departure->target, pos, true, departure->cancelled);
			}
			tft.setTextColor(activeDesign->foregroundColor);
			tft.setTextSize(2);
			tft.setCursor(tft.width() - 7 * 6 * 2, pos);
			if (strcmp("", departure->newPlatform) && strcmp(departure->platform, departure->newPlatform)) {
				tft.setTextColor(activeDesign->highlightColor);
				//tft.print("->");
				tft.print(departure->newPlatform);
			} else {
				tft.print(departure->platform);
			}
			break;
		}
		case DESIGN_LED:
			tft.fillRect(0, pos - 1, tft.width(), 17, activeDesign->backgroundColor);
			tft.setTextColor(activeDesign->foregroundColor);
			tft.setTextSize(2);
			tft.setCursor(2, pos);
			const char padding[] = "   ";
			//snprintf(buf, sizeof(buf), "%*.*s%s", 3, 3, padding, departure->textline);
			snprintf(buf, 5, "% 4s", departure->textline);
			//buf[4] = 0;
			tft.print(buf);
			printScroll(departure->target, pos, true, departure->cancelled);
			tft.setCursor(tft.width() - 5 * 12, pos);
			snprintf(buf, sizeof(buf), "%02d:%02d", hour(departure->realTime), minute(departure->realTime));
			tft.print(buf);
			break;
	}
	pos += activeDesign->itemHeight;
	return true;
}

void printScroll(String text, uint16_t y, bool force, bool cancelled) {
#ifdef TEXT_FILL
	tft.setTextColor(activeDesign->foregroundColor, activeDesign->backgroundColor);
#else // TEXT_FILL
	tft.setTextColor(activeDesign->foregroundColor);
#endif // TEXT_FILL
	tft.setTextSize(2);
	if (text.length() > activeDesign->maxTextLength || force) {
		uint32_t p = 0;
		if (scrollingEnabled && activeDesign->canScroll) {
			p = scroll;
			int16_t ts = text.length() - activeDesign->maxTextLength;
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
		} else {
			if (!force) return;
		}
#ifndef TEXT_FILL
		tft.fillRect(activeDesign->targetX - 2, y - 1, activeDesign->maxTextLength * 6 * 2, activeDesign->itemHeight, activeDesign->backgroundColor);
#endif // TEXT_FILL
		tft.setCursor(activeDesign->targetX, y);
		text = text.substring(p, p + activeDesign->maxTextLength);
		tft.print(text);
		switch (activeDesign->id) {
		case DESIGN_WIDE:
		case DESIGN_LED:
			if (cancelled) {
				uint8_t len = text.length();
				tft.drawFastHLine(activeDesign->targetX, y + 6, (len * 6 - 1) * 2, activeDesign->foregroundColor);
				tft.drawFastHLine(activeDesign->targetX, y + 7, (len * 6 - 1) * 2, activeDesign->foregroundColor);
			}
			break;
		default:
			break;
		}
	}
}

#ifdef ENABLEOTA
void updateStarted() {
	updateInhibit = true;
	Serial.println("Update started...");
	tft.fillScreen(COLOR_BLACK);
	tft.setTextColor(COLOR_WHITE);
	tft.setCursor(0, 0);
	tft.setFont();
	tft.setTextSize(2);
	tft.println("Update...");
}
#endif

#ifdef AUTOCONFIG
WiFiManager wifiManager;
WiFiManagerParameter stationNameParameter("stationName", "Stationsname", fromStationName, sizeof(fromStationName));
WiFiManagerParameter rotationParameter("rotation", "Rotation (1 oder 3)", String(rotation).c_str(), 1);
#ifdef ESP8266
WiFiManagerParameter ldrParameter("ldrpin", "Lichtsensor (-1 zum Deaktivieren, 17 sonst)", String(ldr_pin).c_str(), 2);
#else
WiFiManagerParameter ldrParameter("ldrpin", "Lichtsensor (-1 zum Deaktivieren, CYD z.B. 34 zum Aktivieren)", String(ldr_pin).c_str(), 2);
#endif
#ifdef SCROLLING_ENABLED
WiFiManagerParameter scrollingParameter("scrolling", "Lange Stationen scrollen (1 zum Aktivieren)", String(scrollingEnabled).c_str(), 1);
#endif
String maxDesignString = String("Design-ID (min 0, max ") + String(MAX_DESIGNS - 1) + ")";
WiFiManagerParameter designParameter("design", (maxDesignString).c_str(), String(activeDesign->id).c_str(), 2);
WiFiManagerParameter filterICEParameter("filterICE", "Hochgeschwindigkeitsz&uuml;ge (1 zum Aktivieren)", String((filter & PROD_ICE) > 0).c_str(), 1);
WiFiManagerParameter filterICECParameter("filterICEC", "Intercity- und Eurocityz&uuml;ge 1 zum Aktivieren)", String((filter & PROD_IC_EC) > 0).c_str(), 1);
WiFiManagerParameter filterIRParameter("filterIR", "Interregio- und Schnellz&uuml;ge (1 zum Aktivieren)", String((filter & PROD_IR) > 0).c_str(), 1);
WiFiManagerParameter filterREParameter("filterRE", "Nahverkehr, sonstige Z&uuml;ge (1 zum Aktivieren)", String((filter & PROD_RE) > 0).c_str(), 1);
WiFiManagerParameter filterSParameter("filterS", "S-Bahnen (1 zum Aktivieren)", String((filter & PROD_S) > 0).c_str(), 1);
WiFiManagerParameter filterBUSParameter("filterBUS", "Busse (1 zum Aktivieren)", String((filter & PROD_BUS) > 0).c_str(), 1);
WiFiManagerParameter filterSHIPParameter("filterSHIP", "Schiffe (1 zum Aktivieren)", String((filter & PROD_SHIP) > 0).c_str(), 1);
WiFiManagerParameter filterUParameter("filterU", "U-Bahnen (1 zum Aktieren)", String((filter & PROD_U) > 0).c_str(), 1);
WiFiManagerParameter filterSTBParameter("filterSTB", "Stra&szlig;enbahnen (1 zum Aktivieren)", String((filter & PROD_STB) > 0).c_str(), 1);
WiFiManagerParameter filterASTParameter("filterAST", "Anrufpflichtige Verkehre (1 zum Aktivieren)", String((filter & PROD_AST) > 0).c_str(), 1);
bool shouldSaveConfig = false;

void configModeCallback (WiFiManager *myWiFiManager) {
	if (strlen(errorMessage)) {
		tft.fillScreen(COLOR_RED);
	} else {
		tft.fillScreen(COLOR_BLACK);
	}
	tft.setTextColor(COLOR_WHITE);
	tft.setFont();
	tft.setTextSize(2);
	tft.setCursor(0, 0);
	if (strlen(errorMessage)) {
		tft.println(errorMessage);
	}
	tft.println("Zur Konfiguration mit\nAccess-Point verbinden:");
	tft.println(apSSID);
	tft.println(apPassword);
	if (WiFi.isConnected()) {
		tft.println("Oder im lokalen WLAN:");
		tft.println(WiFi.localIP());
	}
}

void failedConfig() {
	tft.fillScreen(COLOR_RED);
	tft.setTextColor(COLOR_WHITE);
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
	Serial.println("Reading saved Paramters.");
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
			Serial.println("File /station not open");
		}
	} else {
		Serial.println("File /station not found");
	}
	if (LittleFS.exists("/rotation")) {
		File f = LittleFS.open("/rotation", "r");
		if (f) {
			uint8_t r = f.read() - '0';
			if (r == 1 || r == 3) {
				rotation = r;
			}
		} else {
			Serial.println("File /rotation not open");
		}
	} else {
		Serial.println("File /rotation not found");
	}
	if (LittleFS.exists("/ldr")) {
		File f = LittleFS.open("/ldr", "r");
		if (f) {
			ldr_pin = f.readString().toInt();
		} else {
			Serial.println("File /ldr not open");
		}
	} else {
		Serial.println("File /ldr not found");
	}
#ifdef SCROLLING_ENABLED
	if (LittleFS.exists("/scrolling")) {
		File f = LittleFS.open("/scrolling", "r");
		if (f) {
			scrollingEnabled = f.read();
		} else {
			Serial.println("File /scrolling not open");
		}
	} else {
		Serial.println("File /scrolling not found");
	}
#endif
	if (LittleFS.exists("/design")) {
		File f = LittleFS.open("/design", "r");
		if (f) {
			uint8_t d = f.read();
			if (d < MAX_DESIGNS) {
				activeDesign = &designs[d];
			}
		} else {
			Serial.println("File /design not open");
		}
	} else {
		Serial.println("File /design not found");
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
			Serial.println("File /filter not open");
		}
	} else {
		Serial.println("File /filter not found");
	}
	Serial.println("Done loading settings.");
}

bool doReconfig() {
	bool state = wifiManager.startConfigPortal(apSSID, apPassword);
	afterConfigCallback();
	drawStaticContent();
	updateInhibit = false;
	nextCheck = 0;
	errorMessage[0] = 0; // delete error message, set next time if neccessary
	return state;
}

void checkConfigRequest() {
	if (!digitalRead(BUT_IN)) {
		doReconfig();
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
			fromStation = db.getStation(fromStationName);
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
		ldr_pin = atoi(ldrParameter.getValue());
		{
			File f = LittleFS.open("/ldr", "w");
			f.print(ldrParameter.getValue());
			f.close();
		}
#ifdef SCROLLING_ENABLED
		scrollingEnabled = scrollingParameter.getValue()[0] == '1';
		{
			File f = LittleFS.open("/design", "w");
			f.write(scrollingEnabled);
			f.close();
		}
#endif
		uint8_t d = atoi(designParameter.getValue());
		if (d < MAX_DESIGNS) {
			File f = LittleFS.open("/design", "w");
			f.write(d);
			f.close();
			activeDesign = &designs[d];
		}
		ldr_pin = atoi(ldrParameter.getValue());
		{
			File f = LittleFS.open("/ldr", "w");
			f.print(ldrParameter.getValue());
			f.close();
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
	ldrParameter.setValue(String(ldr_pin).c_str(), 2);
	wifiManager.addParameter(&ldrParameter);
#ifdef SCROLLING_ENABLED
	scrollingParameter.setValue(String(scrollingEnabled).c_str(), 1);
	wifiManager.addParameter(&scrollingParameter);
#endif
	designParameter.setValue(String(activeDesign->id).c_str(), 2);
	wifiManager.addParameter(&designParameter);
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
	tft.fillScreen(COLOR_BLACK);
	tft.setTextColor(COLOR_WHITE);
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
	// Retry if no data available
	for (uint8_t i = 0; i < 3 && fromStation == NULL; i++) {
		fromStation = db.getStation(fromStationName);
	}
	yield();
	timeClient.begin();
	timeClient.setTimeOffset(3600); // CET
	adjustTime(3600);
	setSyncProvider(getNtpTime);
	drawStaticContent();
}

void loop() {
	if (nextTime < millis() && !updateInhibit) {
		timeClient.update();
		time_t tnow = timeClient.getEpochTime();
		tdst = dst(tnow);
		if (old_time / 60 != tdst / 60) {
			drawTime();
		}
		nextTime = millis() + 1000;
	}
	if (nextCheck < millis() && !updateInhibit) {
		if (WiFi.status() != WL_CONNECTED) {
			Serial.println("Disconnected");
			failedConnection++;
		} else {
			failedConnection = 0;
		}
		// Try to refetch station
		if (fromStation == NULL) {
			fromStation = db.getStation(fromStationName);
		}
		// Reconfigure if station is still unknown
		if (fromStation == NULL) {
			strncpy(errorMessage, "Station konnte nicht\nabgerufen werden.", sizeof(errorMessage));
			// No changes made, connection issue? Try to restart.
			if (doReconfig()) {
				ESP.restart();
			}
		}
		Serial.println("Reload");
		da = db.getDepartures(fromStation->stationId, NULL, tdst, activeDesign->maxEntries, 3, filter);
		Serial.println();
		departure = da;
		if (departure != NULL) {
			failedConnection = 0; // Reset, as we got a result anyways.
			failedUpdates = 0;
		} else {
			failedUpdates++;
			Serial.print("Update failed, currently at ");
			Serial.println(failedUpdates);
		}
		uint16_t pos = activeDesign->headerOffset;
		while (departure != NULL) {
			if (!drawDeparture(departure, pos)) break;
			departure = departure->next;
		}
		tft.setTextColor(COLOR_RED);
		// clear empty spots (not enough departures) only if we got data.
		while (pos + activeDesign->itemHeight * 2 - 2 <= tft.height() && da != NULL) {
			pos += activeDesign->itemHeight;
			tft.fillRect(0, pos - 1, tft.width(), 18, activeDesign->backgroundColor);
		}
		if (failedConnection == 5) {
			tft.fillScreen(COLOR_RED);
			tft.setTextColor(COLOR_WHITE);
			tft.setFont();
			tft.setTextSize(2);
			tft.setCursor(0, 0);
			tft.println("WLAN Verbindung fehl\n-geschlagen");
			tft.print("Starte neu...");
			delay(5000);
			ESP.restart();
		}
		nextCheck = millis() + 50000;
	}
	if (scrollingEnabled && activeDesign->canScroll && nextScroll < millis() && !updateInhibit && da != NULL) {
		departure = da;
		uint16_t pos = activeDesign->headerOffset;
		while (departure != NULL && pos + activeDesign->itemHeight - 2 < tft.height()) {
			printScroll(departure->target, pos, false, departure->cancelled);
			pos += activeDesign->itemHeight;
			departure = departure->next;
		}
		scroll++;
		nextScroll = millis() + SCROLL_INTERVAL;
	}
#ifdef ENABLEOTA
	ArduinoOTA.handle();
#endif
#ifdef AUTOCONFIG
	checkConfigRequest();
#endif
	if (ldr_pin > 0) { //0 is never an analog in
		if (nextBrightness < millis()) {
			uint16_t b = analogRead(ldr_pin);
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
