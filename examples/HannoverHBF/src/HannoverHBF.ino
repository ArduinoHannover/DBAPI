#include <ESP8266WiFi.h>
#include <DBAPI.h>
const char* ssid = "SSID";
const char* password = "password";

DBAPI db;

void setup() {
	Serial.begin(115200);
	WiFi.mode(WIFI_STA);
	WiFi.begin(ssid, password);
	while (WiFi.status() != WL_CONNECTED) {
		Serial.write('.');
		delay(500);
	}
	Serial.println();
	DBstation* station = db.getStation("Hannover Hbf");
	yield();
	if (station != NULL) {
		Serial.println();
		Serial.print("Name:      ");
		Serial.println(station->name);
		Serial.print("ID:        ");
		Serial.println(station->stationId);
		Serial.print("Latitude:  ");
		Serial.println(station->latitude);
		Serial.print("Longitude: ");
		Serial.println(station->longitude);
		DBdeparr* da = db.getDepartures(station->stationId, NULL, 0, 20, 1, PROD_ICE | PROD_IC_EC | PROD_IR | PROD_RE | PROD_S);
		while (da != NULL) {
			yield();
			Serial.println();
			Serial.print("Date:     ");
			char buf[11];
			snprintf(buf, sizeof(buf), "%02d.%02d.%4d", day(depature->time), month(depature->time), year(depature->time) + 1970);
			Serial.println(buf);
			Serial.print("Time:     ");
			snprintf(buf, sizeof(buf), "%02d:%02d", hour(depature->time), minute(depature->time));
			Serial.println(buf);
			Serial.print("Realtime: ");
			snprintf(buf, sizeof(buf), "%02d:%02d", hour(depature->realTime), minute(depature->realTime));
			Serial.println(buf);
			Serial.print("Product:  ");
			Serial.println(da->product);
			Serial.print("Line:     ");
			Serial.println(da->line);
			Serial.print("Target:   ");
			Serial.println(da->target);
			Serial.print("Delay:    ");
			Serial.println(da->delay);
			Serial.print("Platform: ");
			Serial.println(da->platform);
			da = da->next;
		}
	}
	//station = db.getStationByCoord(52377222, 9741667);
}

void loop() {
}
