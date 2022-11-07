#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <DBAPI.h>
#include <Adafruit_ILI9341.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TimeLib.h>

const char* ssid = "SSID";
const char* password = "password";
const char* fromStationName = "Hannnover Hbf";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

#define BACKGROUND_COLOR 0x001F // Blue
#define FOREGROUND_COLOR 0xFFFF // White
#define HIGHLIGHT_COLOR  0xFFE0 // Yellow

DBAPI db;
DBstation* fromStation;
DBdeparr* da = NULL;
uint32_t nextCheck;

#define TFT_DC 4
#define TFT_CS 15
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC);

void setup() {
	Serial.begin(115200);
	tft.begin();
	WiFi.mode(WIFI_STA);
	WiFi.begin(ssid, password);
	tft.fillScreen(BACKGROUND_COLOR);
	tft.setTextColor(FOREGROUND_COLOR);
	tft.setRotation(1);
	tft.print("Verbinde...");
 	Serial.print("Verbinde...");
	while (WiFi.status() != WL_CONNECTED) {
		tft.write('.');
    	Serial.write('.');
		delay(500);
	}
	db.setAGFXOutput(true);
	Serial.println();
	fromStation = db.getStation(fromStationName);
	yield();
	timeClient.begin();
	timeClient.setTimeOffset(3600); // CET
}

uint8_t s;
void loop() {
	if (nextCheck < millis()) {
		if (WiFi.status() != WL_CONNECTED) {
			Serial.println("Disconnected");
		}
		Serial.println("Reload");
		da = db.getDepatures(fromStation->stationId, NULL, NULL, NULL, 11, PROD_RE | PROD_S);
		timeClient.update();
		time_t tnow = timeClient.getEpochTime();
		time_t tdst = dst(tnow);
		s = 0;
		Serial.println();
		tft.fillScreen(BACKGROUND_COLOR);
		tft.setTextColor(FOREGROUND_COLOR);
		tft.setTextSize(2);
		tft.setCursor(2, 0);
		if (hour(tdst) < 10) tft.write('0');
		tft.print(hour(tdst));
		tft.write(':');
		if (minute(tdst) < 10) tft.write('0');
		tft.print(minute(tdst));
		tft.setCursor((tft.width() - (strlen(fromStationName) * 6 - 1) * 2) / 2, 0);
		tft.print(fromStationName);
		tft.fillRect(0, 16, tft.width(), 17, HIGHLIGHT_COLOR);
		tft.setTextColor(BACKGROUND_COLOR);
		tft.setCursor(2, 17);
		tft.print("Zeit");
		tft.setCursor(9 * 6 + 6, 17);
		tft.print("Nach");
		tft.setCursor(tft.width() - 7 * 6 * 2, 17);
		tft.print("Gleis");
		uint16_t pos = 18;
	    while (da != NULL) {
			Serial.println(s);
			pos += 18;
			if (pos + 16 > tft.height()) break;
			tft.fillRect(0, pos - 1, 9 * 6 + 4 , 17, FOREGROUND_COLOR);
			tft.setTextColor(BACKGROUND_COLOR);
			tft.setTextSize(1);
			tft.setCursor(2, pos);
			tft.print(da->time);
			tft.setCursor(2, pos + 8);
			tft.print(da->product);
			if (strcmp("", da->textline) != 0) {
				tft.write(' ');
				tft.print(da->textline);
			}
			tft.setTextColor(FOREGROUND_COLOR);
			tft.setCursor(9 * 6 + 6, pos);
			tft.print(da->target);
			tft.setTextColor(HIGHLIGHT_COLOR);
			tft.setCursor(9 * 6 + 6, pos + 8);
			if (strcmp("cancel", da->textdelay) == 0) {
				tft.print("F\x84llt heute aus");
				tft.drawFastHLine(9 * 6 + 6, pos + 3, strlen(da->target) * 6 - 1, FOREGROUND_COLOR);
			} else if (strcmp("0", da->textdelay) != 0 && strcmp("-", da->textdelay) != 0) {
				tft.print("ca. ");
				tft.print(da->textdelay);
				tft.print(" sp\x84ter");
			}
			tft.setTextColor(FOREGROUND_COLOR);
			tft.setTextSize(2);
			tft.setCursor(tft.width() - 7 * 6 * 2, pos);
			if (strcmp("", da->newPlatform) != 0) {
				tft.setTextColor(HIGHLIGHT_COLOR);
				tft.print(da->newPlatform);
			} else {
				tft.print(da->platform);
			}
		}
		nextCheck = millis() + 50000;
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
			if (hour(t) < 3) { //Time before 2AM without DST (3AM DST, wich doesn't exist)
				return t + 3600;
			}
		}
	}
	return t;
}
