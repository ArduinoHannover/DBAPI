#include "DBAPI.h"
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#ifdef DEBUG_ESP_PORT
#define DB_DEBUG_MSG(...) DEBUG_ESP_PORT.printf( __VA_ARGS__ )
#else
#define DB_DEBUG_MSG(...)
#endif

DBAPI::DBAPI() {

}


DBstation* DBAPI::getStation(
		const char* name,
		const char* address,
		uint8_t     num
	) {
	while (stations != NULL) {
		DBstation* next = stations->next;
		free(stations);
		stations = next;
	}
	/*
	bool isStation;
	if (name != NULL) {
		isStation = true;
	} else if (address != NULL) {
		isStation = false;
	} else {
		return NULL;
	}
	*/
	WiFiClientSecure client;
	client.setInsecure(); // Don't check fingerprint
	if (!client.connect(host, 443)) {
		DB_DEBUG_MSG("DBAPI: Connection to Host failed.\n");
		return NULL;
	}
	String json = String("{\"locationTypes\":[\"ST\"],\"searchTerm\":\"") + name + "\"}";
	client.print(String("POST /mob/location/search HTTP/1.1\r\nConnection: close\r\nX-Correlation-ID: Arduino\r\nContent-Type: application/x.db.vendo.mob.location.v3+json\r\nAccept: application/x.db.vendo.mob.location.v3+json\r\nHost: ") + host +
		"\r\nContent-Length: " + json.length() + "\r\n\r\n" +
		json
		);
	
	char endOfHeaders[] = "\r\n\r\n";
	if (!client.find(endOfHeaders)) {
		DB_DEBUG_MSG("Did not find headers\n");
		return stations;
	}
	JsonDocument doc;
	DeserializationError error = deserializeJson(doc, client);
	if (error) {
		DB_DEBUG_MSG("deserializeJson() on Station failed");
		DB_DEBUG_MSG(error.f_str());
		return stations;
	}
	DBstation* prev = NULL;
	for (uint8_t i = 0; i < doc.size(); i++) {
		DBstation* station = new DBstation();
		JsonObject st = doc[i];
		strncpy(station->name, st["name"], sizeof(station->name));
		strncpy(station->stationId, st["locationId"], sizeof(station->stationId));
		station->longitude = st["coordinates"]["longitude"];
		station->latitude = st["coordinates"]["latitude"];
		station->next = NULL;
		if (prev == NULL) { // First element
			stations = station;
		} else {
			prev->next = station;
		}
		prev = station;
	}
	return stations;
}

DBstation* DBAPI::getStationByCoord(
		uint32_t latitude,
		uint32_t longitude,
		uint8_t  num,
		uint16_t maxDistance
	) {
	return NULL;
}

DBdeparr* DBAPI::getStationBoard(
		const char  type[8],
		const char* stationId,
		const char* target,
		const char* Dtime,
		const char* Ddate,
		uint8_t     num,
		uint16_t    productFilter
	) {
	while (deparr != NULL) {
		DBdeparr* next = deparr->next;
		free(deparr);
		deparr = next;
	}
	WiFiClientSecure client;

	{
		JsonDocument doc;
		JsonArray verkehrsmittel = doc["verkehrsmittel"].to<JsonArray>();
		doc["ursprungsBahnhofId"] = stationId;
		if (Dtime != NULL) {
			doc["anfragezeit"] = Dtime;
		}
		if (Ddate != NULL) {
			doc["datum"] = Ddate;
		}
		for (uint8_t i = 0; i < 10; i++) {
			if (productFilter & (1 << (9 - i))) {
				verkehrsmittel.add(services[i]);
			}
		}
		size_t outputCapacity = 350;
		char* output = (char*)calloc(outputCapacity, sizeof(char));
		serializeJson(doc, output, outputCapacity);

		client.setInsecure(); // Don't check fingerprint
		if (!client.connect(host, 443)) {
			DB_DEBUG_MSG("DBAPI: Connection to Host failed.\n");
			return NULL;
		}
		
		DB_DEBUG_MSG("DBAPI: Requesting StationBoard.\n");
		client.print(String("POST /mob/bahnhofstafel/") + type + " HTTP/1.1\r\n" +
			"Host: " + host + "\r\n" +
			"Content-Type: application/x.db.vendo.mob.bahnhofstafeln.v2+json\r\n" +
			"Accept: application/x.db.vendo.mob.bahnhofstafeln.v2+json\r\n" +
			"X-Correlation-ID: Arduino\r\n" +
			"Content-Length: " + strlen(output) + "\r\n" +
			"Connection: close\r\n\r\n" + output);
		free(output);
		char endOfHeaders[] = "\r\n\r\n";
		if (!client.find(endOfHeaders)) {
			DB_DEBUG_MSG("Did not find headers\n");
			return deparr;
		}
	}
	bool abfahrt = strcmp(type, "abfahrt") == 0;
	JsonDocument doc;
	client.find(":["); // Skip to first element
	DBdeparr* prev = NULL;
	uint8_t cnt = 0;
	do {
		DeserializationError error = deserializeJson(doc, client);
		if (error) {
			DB_DEBUG_MSG("deserializeJson() on departures/arrivals failed");
			DB_DEBUG_MSG(error.f_str());
			return deparr;
		}
		DBdeparr* da = new DBdeparr();
		String targ = doc[abfahrt?"richtung":"abgangsOrt"];
		
		switch (repum) {
			case REP_AGFX:
				targ.replace("ß", "\xE0");
				targ.replace("Ä", "\x8E");
				targ.replace("Ö", "\x99");
				targ.replace("Ü", "\x9A");
				targ.replace("ä", "\x84");
				targ.replace("ö", "\x94");
				targ.replace("ü", "\x81");
				break;
			case REP_UML:
				targ.replace("ß", "ss");
				targ.replace("Ä", "Ae");
				targ.replace("Ö", "Oe");
				targ.replace("Ü", "Ue");
				targ.replace("ä", "ae");
				targ.replace("ö", "oe");
				targ.replace("ü", "ue");
				break;
			default:
				break;
		}
		targ.toCharArray(da->target, sizeof(DBdeparr::target));
		strncpy(da->product, doc["kurztext"], sizeof(DBdeparr::product));
		//strncpy(da->product, doc["produktGattung"], sizeof(DBdeparr::product));
		strncpy(da->textline, doc["mitteltext"], sizeof(DBdeparr::textline));
		for (uint8_t i = 0; i < strlen(da->textline); i++) {
			if (da->textline[i] >= '0' && da->textline[i] <= '9') {
				strncpy(da->textline, &doc["mitteltext"].as<const char*>()[i], sizeof(DBdeparr::textline));
				da->line = atol(da->textline);
				break;
			}
		}
		if (!doc["gleis"].isNull()) {
			strncpy(da->platform, doc["gleis"], sizeof(DBdeparr::platform));
		} else {
			da->platform[0] = 0;
		}
		
		if (!doc["ezGleis"].isNull()) {
			strncpy(da->newPlatform, doc["ezGleis"], sizeof(DBdeparr::newPlatform));
		} else {
			da->newPlatform[0] = 0;
		}
		JsonArray arr = doc["echtzeitNotizen"];
		const char* cancellation = "Halt entfällt";
		da->cancelled = false;
		for (uint8_t i = 0; i < arr.size(); i++) {
			if (arr[i]["text"].as<String>().equals(cancellation)) {
				da->cancelled = true;
			} else {
				//Serial.println(arr[i]["text"].as<String>());
			}
		}
		da->time = this->parseTime(String(doc[abfahrt ? "abgangsDatum" : "ankunftsDatum"]));
		if (doc[abfahrt ? "ezAbgangsDatum" : "ezAnkunftsDatum"].isNull()) {
			da->realTime = da->time;
		} else {
			da->realTime = this->parseTime(String(doc[abfahrt ? "ezAbgangsDatum" : "ezAnkunftsDatum"]));
		}
		da->delay = da->realTime - da->time;
		da->delay /= 60;

		da->next = NULL;
		if (prev == NULL) {
			DB_DEBUG_MSG("DBAPI: Got first departure.");
			deparr = da;
		} else {
			DB_DEBUG_MSG("DBAPI: Got next departure.");
			prev->next = da;
		}
		prev = da;
		cnt++;
	} while (client.findUntil(",","]") && cnt < num);
	return deparr;
}

time_t DBAPI::parseTime(String t) {
	tmElements_t time;
	// 0123-56-89T12:45:67+90:23
	time.Year = atoi(t.substring(0,4).c_str()) - 1970;
	time.Month = atoi(t.substring(5,7).c_str());
	time.Day = atoi(t.substring(8,10).c_str());
	time.Hour = atoi(t.substring(11,13).c_str());
	time.Minute = atoi(t.substring(14,16).c_str());
	time.Second = 0;
	return makeTime(time);
}

DBdeparr* DBAPI::getDepartures(
		const char* stationId,
		const char* target,
		const char* Dtime,
		const char* Ddate,
		uint8_t     num,
		uint16_t    productFilter
	) {
	return getStationBoard("abfahrt", stationId, target, Dtime, Ddate, num, productFilter);
}

DBdeparr* DBAPI::getArrivals(
		const char* stationId,
		const char* target,
		const char* Dtime,
		const char* Ddate,
		uint8_t     num,
		uint16_t    productFilter
	) {
	return getStationBoard("ankunft", stationId, target, Dtime, Ddate, num, productFilter);
}

void DBAPI::setAGFXOutput(bool gfx) {
	repum = gfx ? REP_AGFX : REP_UML;
}

void DBAPI::setUmlaut(enum DBumlaut uml) {
	repum = uml;
}

const char* DBAPI::services[] = {
	"HOCHGESCHWINDIGKEITSZUEGE",
	"INTERCITYUNDEUROCITYZUEGE",
	"INTERREGIOUNDSCHNELLZUEGE",
	"NAHVERKEHRSONSTIGEZUEGE",
	"SBAHNEN",
	"BUSSE",
	"SCHIFFE",
	"UBAHN",
	"STRASSENBAHN",
	"ANRUFPFLICHTIGEVERKEHRE"
};