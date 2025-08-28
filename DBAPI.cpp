#include "DBAPI.h"
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Hash.h>

#ifdef DEBUG_ESP_PORT
#define DB_DEBUG_MSG(...) DEBUG_ESP_PORT.printf( __VA_ARGS__ )
#else
#define DB_DEBUG_MSG(...)
#endif

DBAPI::DBAPI() {

}

/**
 * Get stations matching the name or close to an address
 * @param name    Name of the station to get
 * @param address Address to request nearest stations from - currently not implemented, leave as NULL
 * @param num     Maxmium of stations to request
 * @return DBstation* array, possibly NULL if no results were found
 */
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
		DB_DEBUG_MSG(error.c_str());
		return stations;
	}
	DBstation* prev = NULL;
	for (uint8_t i = 0; i < doc.size(); i++) {
		DBstation* station = new DBstation();
		JsonObject st = doc[i];
		String stationname = st["name"];
		switch (repum) {
			case REP_AGFX:
				stationname.replace("ß", "\xE0");
				stationname.replace("Ä", "\x8E");
				stationname.replace("Ö", "\x99");
				stationname.replace("Ü", "\x9A");
				stationname.replace("ä", "\x84");
				stationname.replace("ö", "\x94");
				stationname.replace("ü", "\x81");
				break;
			case REP_UML:
				stationname.replace("ß", "ss");
				stationname.replace("Ä", "Ae");
				stationname.replace("Ö", "Oe");
				stationname.replace("Ü", "Ue");
				stationname.replace("ä", "ae");
				stationname.replace("ö", "oe");
				stationname.replace("ü", "ue");
				break;
			default:
				break;
		}
		strncpy(station->name, stationname.c_str(), sizeof(station->name));
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

/**
 * Get stations by coordinates
 * Legacy function, not implemented again
 * @return NULL currently
 */
DBstation* DBAPI::getStationByCoord(
		uint32_t latitude,
		uint32_t longitude,
		uint8_t  num,
		uint16_t maxDistance
	) {
	return NULL;
}

/**
 * Requests departures/arrivals from/at given stationID.
 * @param type abfahrt or ankunft
 * @param stationId ID of stations, can be acquired by getStation(...)->stationId
 * @param target target of service, currently not used, leave as NULL
 * @param time specify request date/time, leave at 0 if you want to query with current time
 * @param maxCount maxiumum of services to request (may be lower if maxDuration is reached before maxCount)
 * @param maxDuration maximum of hours to request (each hour will possibly generate a new http request)
 * @param productFilter any combination of DBprod values for service types to request
 * @return DBdeparr array, possibly NULL if no service is available with the requested parameters or an error occured
 */
DBdeparr* DBAPI::getStationBoard(
		const char  type[8],
		const char* stationId,
		const char* target,
		time_t      time,
		uint8_t     maxCount,
		uint8_t     maxDuration,
		uint16_t    productFilter
	) {
	// sanity check, if no station is supplied, a request would crash ArduinJSON later.
	if (stationId == NULL || !strlen(stationId)) {
		return NULL;
	}
	while (deparr != NULL) {
		DBdeparr* next = deparr->next;
		free(deparr);
		deparr = next;
	}

	bool abfahrt = strcmp(type, "abfahrt") == 0;
	DBdeparr* prev = NULL;
	uint8_t cnt = 0;
	uint8_t hashes[maxCount][20];

	// Set static request parameters
	JsonDocument reqDoc;
	JsonArray verkehrsmittel = reqDoc["verkehrsmittel"].to<JsonArray>();
	for (uint8_t i = 0; i < 10; i++) {
		if (productFilter & (1 << (9 - i))) {
			verkehrsmittel.add(services[i]);
		}
	}
	reqDoc["ursprungsBahnhofId"] = stationId;

	for (uint8_t offset = 0; offset < maxDuration && cnt < maxCount; offset++) {
		// Get current time, if requesting more than one hour and no request time is set
		// Sanity check, that year is > 2020, so only successful synced time is used
		// Your device would probably not survive 2020 years without restart, huh?
		if (time == 0 && maxDuration > 1 && year() > 2020) {
			time = now();
		}
		// Set params
		if (time != 0) {
			// Nobody knows, what happens at DST switch... ¯\_(ツ)_/¯
			char buf[11];
			// Setting the time offset - this might generate duplicates.
			// Train would have departed in queried hour but has delay of 70 min:
			// Will now be shown in the currently queried hour as the regular time matches.
			// But also in the next hour, because it will be reachable due to the delay.
			uint32_t tr = time + offset * 3600;
			snprintf(buf, 11, "%02d:%02d", hour(tr), minute(tr));
			reqDoc["anfragezeit"] = buf;
			snprintf(buf, 11, "%4d-%02d-%02d", year(tr), month(tr), day(tr));
			reqDoc["datum"] = buf;
		}
		size_t outputCapacity = 350;
		char* output = (char*)calloc(outputCapacity, sizeof(char));
		serializeJson(reqDoc, output, outputCapacity);

		// Init new client for each request
		WiFiClientSecure client;
		client.setInsecure(); // Don't check fingerprint
		if (!client.connect(host, 443)) {
			DB_DEBUG_MSG("DBAPI: Connection to Host failed.\n");
			free(output);
			return NULL;
		}
		
		DB_DEBUG_MSG("DBAPI: Requesting StationBoard.\n");
		DB_DEBUG_MSG(output);
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
		JsonDocument doc;
		client.find(":["); // Skip to first element
		do {
			DeserializationError error = deserializeJson(doc, client);
			if (error) {
				DB_DEBUG_MSG("deserializeJson() on departures/arrivals failed");
				DB_DEBUG_MSG(error.c_str());
				return deparr;
			}

			uint8_t hash[20];
			// hash to save RAM as the ID is >150 chars
			sha1(doc["zuglaufId"].as<String>(), hash);

			bool match = false;
			for (uint8_t i = 0; i < cnt && !match; i++) {
				bool mismatch = false;
				for (uint8_t j = 0; j < 20; j++) {
					if (hashes[i][j] != hash[j]) {
						mismatch = true;
						break;
					}
					match |= !mismatch;
				}
			}
			// duplicate entry found, skipping
			if (match) continue;
			memcpy(hashes[cnt], hash, sizeof(hash));

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
			da->cancelled = false;
			for (uint8_t i = 0; i < arr.size(); i++) {
				if (arr[i]["text"].as<String>().equals("Halt entfällt")) {
					da->cancelled = true;
					break; // Not interested in other notes right now
				} else {
					//Serial.println(arr[i]["text"].as<String>());
				}
			}
			da->time = this->parseTime(doc[abfahrt ? "abgangsDatum" : "ankunftsDatum"]);
			if (doc[abfahrt ? "ezAbgangsDatum" : "ezAnkunftsDatum"].isNull()) {
				da->realTime = da->time;
			} else {
				da->realTime = this->parseTime(doc[abfahrt ? "ezAbgangsDatum" : "ezAnkunftsDatum"]);
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
		} while (client.findUntil(",","]") && cnt < maxCount);

		if (maxDuration > 0 && time == 0 && prev != NULL) {
			// If no synced time available, set request time to last known depature - 59 minutes
			time = prev->time - 3540;
		}
		if (year(time) < 2020) break; // Can't query more hours, as there is no valid timestamp
	}
	return deparr;
}

/**
 * Helper to parse string to time_t
 * @param t Time string in time with potential TZ formatted like 0123-56-89T12:45:67+90:23
 * @return time_t timestamp
 */
time_t DBAPI::parseTime(const char* t) {
	tmElements_t time;
	// 0123-56-89T12:45:67+90:23
	// Year in timeElements is years since 1970
	time.Year = atoi(&t[0]) - 1970;
	time.Month = atoi(&t[5]);
	time.Day = atoi(&t[8]);
	time.Hour = atoi(&t[11]);
	time.Minute = atoi(&t[14]);
	time.Second = 0;
	return makeTime(time);
}

/**
 * Wrapper for querying departures
 * @see getStationBoard
 */
DBdeparr* DBAPI::getDepartures(
		const char* stationId,
		const char* target,
		time_t      time,
		uint8_t     maxCount,
		uint8_t     maxDuration,
		uint16_t    productFilter
	) {
	return getStationBoard("abfahrt", stationId, target, time, maxCount, maxDuration, productFilter);
}

/**
 * Wrapper for querying arrivals
 * @see getStationBoard
 */
DBdeparr* DBAPI::getArrivals(
		const char* stationId,
		const char* target,
		time_t      time,
		uint8_t     maxCount,
		uint8_t     maxDuration,
		uint16_t    productFilter
	) {
	return getStationBoard("ankunft", stationId, target, time, maxCount, maxDuration, productFilter);
}

/**
 * Replace special characters for AdafruitGFX charset
 * @param gfx If true, will replace for AdafruitGFX, otherwise like ä -> ae
 */
void DBAPI::setAGFXOutput(bool gfx) {
	repum = gfx ? REP_AGFX : REP_UML;
}

/**
 * Replace special characters for displays
 * @param uml `REP_AGFX` for AdafruitGFX, `REP_UML` for simple ä -> ae and REP_NONE for raw output 
 */
void DBAPI::setUmlaut(enum DBumlaut uml) {
	repum = uml;
}

/**
 * Translate numeric values into strings for 2025+ API version
 */
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