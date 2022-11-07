#include "DBAPI.h"
#include <WiFiClientSecure.h>

#ifdef DEBUG_ESP_PORT
#define DB_DEBUG_MSG(...) DEBUG_ESP_PORT.printf( __VA_ARGS__ )
#else
#define DB_DEBUG_MSG(...)
#endif

DBAPI::DBAPI() {

}

String DBAPI::getXMLParam(String haystack, const char* param) {
	String p = String(param) + "=\"";
	int16_t pos = haystack.indexOf(p);
	if (pos == -1) {
		return String();
	}
	pos += p.length();
	return haystack.substring(pos, haystack.indexOf('"', pos));
}

String DBAPI::getIParam(String haystack, const char* param) {
	String p = String(param) + "=";
	int16_t pos = haystack.indexOf(p);
	if (pos == -1) {
		return String();
	}
	pos += p.length();
	return haystack.substring(pos, haystack.indexOf('@', pos));
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
	bool isStation;
	if (name != NULL) {
		isStation = true;
	} else if (address != NULL) {
		isStation = false;
	} else {
		return NULL;
	}
	WiFiClientSecure client;
	client.setInsecure(); // Don't check fingerprint
	if (!client.connect(host, 443)) {
		DB_DEBUG_MSG("DBAPI: Connection to Host failed.\n");
		return NULL;
	}
	String xml = String("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\r\n<ReqC ver=\"1.1\" prod=\"String\" lang=\"DE\">\r\n") +
		"<LocValReq id=\"001\" maxNr=\"" + num + "\" sMode=\"1\">\r\n" +
		"<ReqLoc type=\"" + (isStation ? "ST" : "ALLTYPE") + "\" match=\"" + (isStation ? name : address) + "\" />\r\n" +
		"</LocValReq>\r\n" +
		"</ReqC>";
	client.print(String("POST /bin/query.exe/dn HTTP/1.1\r\nConnection: close\r\nContent-Type: text/xml\r\nHost: ") + host +
		"\r\nContent-Length: " + xml.length() + "\r\n\r\n" +
		xml
		);
	while (client.connected()) {
		String line = client.readStringUntil('\n');
		if (line == "\r") {
			break;
		}
	}
	DBstation* prev = NULL;
	while (client.available()) {
		while (client.available()) {
			if (client.read() == '<') {
				if (client.read() == 'S') {
					DB_DEBUG_MSG("DBAPI: Parsing new Station.\n");
					break; // hopefully got <S (tation >) beginning
				}
			}
		}
		if (!client.available()) break;
		DBstation* station = new DBstation();
		String st = client.readStringUntil('>');
		getXMLParam(st, "name").toCharArray(station->name, sizeof(DBstation::name));
		getXMLParam(st, "externalStationNr").toCharArray(station->stationId, sizeof(DBstation::stationId));
		station->longitude = atol(getXMLParam(st, "x").c_str());
		station->latitude  = atol(getXMLParam(st, "y").c_str());
		station->next = NULL;
		if (prev == NULL) {
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
	//String("https://reiseauskunft.bahn.de/bin/query.exe/dol?performLocating=2&L=vs_java&look_nv=del_doppelt%7Cyes&look_x=" + longitude + "&look_y=" + latitude + "&look_maxno=" + num + "0&look_maxdist=" + maxDistance);
	return NULL;
}

DBdeparr* DBAPI::getStationBoard(
		const char  type[4],
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
	String qString;
	if (Dtime != NULL) {
		qString += String("&time=") + Dtime;
		if (Ddate != NULL) {
			qString += String("&date=") + Ddate;
		}
	}
	if (target != NULL) {
		qString += String("&Z=") + target;
	}
	if (num != 0) {
		qString += String("&maxJourneys=") + num;
	}
	
	char products[11];
	for (uint8_t i = 0; i < 10; i++)
		products[i] = productFilter & (1 << (9 - i)) ? '1' : '0';
	products[10] = '\0';
	
	WiFiClientSecure client;
	client.setInsecure(); // Don't check fingerprint
	if (!client.connect(host, 443)) {
		DB_DEBUG_MSG("DBAPI: Connection to Host failed.\n");
		return NULL;
	}
	
	DB_DEBUG_MSG("DBAPI: Requesting StationBoard.\n");
	client.print(String("GET /bin/stboard.exe/dn?start=yes&L=vs_java3&productsFilter=") + products + "&input=" + stationId + "&boardType=" + type + qString + " HTTP/1.1\r\n" +
		"Host: " + host + "\r\n" +
		"Connection: close\r\n\r\n");
	while (client.connected()) {
		String line = client.readStringUntil('\n');
		if (line == "\r") {
			break;
		}
	}
	DBdeparr* prev = NULL;
	while (client.available()) {
		while (client.available()) {
			if (client.read() == '<') {
				if (client.read() == 'J') {
					DB_DEBUG_MSG("DBAPI: Parsing new Journey.\n");
					break; // hopefully got <J (ourney >) beginning
				}
			}
		}
		if (!client.available()) {
			break;
		}
		
		DBdeparr* da = new DBdeparr();
		String journey = client.readStringUntil('>');
		getXMLParam(journey, "fpTime").toCharArray(da->time, sizeof(DBdeparr::time));
		getXMLParam(journey, "fpDate").toCharArray(da->date, sizeof(DBdeparr::date));
		String targ = getXMLParam(journey, "targetLoc");
		targ.replace("&#x0028;", "(");
		targ.replace("&#x0029;", ")");
		// Replace Umlauts for Adafruit GFX
		targ.replace("\xdf", agfx ? "\xE0" : "ss");
		targ.replace("\xc4", agfx ? "\x8E" : "Ae");
		targ.replace("\xd6", agfx ? "\x99" : "Oe");
		targ.replace("\xdc", agfx ? "\x9A" : "Ue");
		targ.replace("\xe4", agfx ? "\x84" : "ae");
		targ.replace("\xf6", agfx ? "\x94" : "oe");
		targ.replace("\xfc", agfx ? "\x81" : "ue");
		targ.toCharArray(da->target, sizeof(DBdeparr::target));
		String prod = getXMLParam(journey, "prod");
		// Extract number from prod tag
		uint8_t chpos = 255;
		for (char c = '1'; c <= '9'; c++) {
			uint8_t pos = prod.indexOf(c);
			if (pos < chpos) {
				chpos = pos;
			}
		}
		if ((uint8_t)prod.indexOf(' ') < chpos) {
			chpos = prod.indexOf(' ');
		}
		// Extract line
		da->line = atol(prod.substring(chpos, prod.indexOf('#')).c_str());
		// Lines could be text like SEV, so also outputting the textform
		// For backwards compatibility not replacing line param
		String textline = prod.substring(chpos, prod.indexOf('#'));
		textline.trim();
		textline.toCharArray(da->textline, sizeof(DBdeparr::textline));
		// Reduce to product name
		prod = prod.substring(0, chpos);
		prod.trim();
		// If no number found, clip #...
		prod.substring(0, prod.indexOf('#')).toCharArray(da->product, sizeof(DBdeparr::product));
		// Extract delay String
		getXMLParam(journey, "delay").toCharArray(da->textdelay, sizeof(DBdeparr::textdelay));
		// Extract numeric delay
		da->delay = atoi(getXMLParam(journey, "e_delay").c_str());
		// Extract platform
		getXMLParam(journey, "platform").toCharArray(da->platform, sizeof(DBdeparr::platform));
		// Extract changed platform if there is any
		getXMLParam(journey, "newpl").toCharArray(da->newPlatform, sizeof(DBdeparr::newPlatform));
		da->next = NULL;
		if (prev == NULL) {
			DB_DEBUG_MSG("DBAPI: Got first depature.");
			deparr = da;
		} else {
			DB_DEBUG_MSG("DBAPI: Got next depature.");
			prev->next = da;
		}
		prev = da;
	}
	return deparr;
}

DBdeparr* DBAPI::getDepatures(
		const char* stationId,
		const char* target,
		const char* Dtime,
		const char* Ddate,
		uint8_t     num,
		uint16_t    productFilter
	) {
	return getStationBoard("dep", stationId, target, Dtime, Ddate, num, productFilter);
}

DBdeparr* DBAPI::getArrivals(
		const char* stationId,
		const char* target,
		const char* Dtime,
		const char* Ddate,
		uint8_t     num,
		uint16_t    productFilter
	) {
	return getStationBoard("arr", stationId, target, Dtime, Ddate, num, productFilter);
}

void DBAPI::setAGFXOutput(bool gfx) {
	agfx = gfx;
}