#ifndef DBAPI_H
#define DBAPI_H

#include <Arduino.h>
#include <WiFiClientSecure.h>

struct DBdeparr {
	char      time[6];
	char      date[9];
	char      textdelay[5];
	uint16_t  delay;
	char      platform[5]; 
	char      target[50];
	char      product[5];
	uint16_t  line;
	DBdeparr* next;
};

struct DBstation {
	char       name[30];
	char       stationId[11];
	uint32_t   longitude;
	uint32_t   latitude;
	DBstation* next;
};

enum DBproduct {
	PROD_ICE     = 1 <<  9,
	PROD_IC_EC   = 1 <<  8,
	PROD_IR      = 1 <<  7,
	PROD_RE      = 1 <<  6,
	PROD_S       = 1 <<  5,
	PROD_BUS     = 1 <<  4,
	PROD_SHIP    = 1 <<  3,
	PROD_U       = 1 <<  2,
	PROD_STB     = 1 <<  1,
	PROD_AST     = 1 <<  0
};

class DBAPI {
	private:
		const char* host = "reiseauskunft.bahn.de";
		String getXMLParam(String haystack, const char* param);
		String getIParam(String haystack, const char* param);
		DBdeparr*  deparr   = NULL;
		DBstation* stations = NULL;
	public:
		DBAPI();
		DBstation* getStation(
			const char* name    = NULL,
			const char* address = NULL,
			uint8_t     num     =   10
		);
		DBstation* getStationByCoord(
			uint32_t latitude,
			uint32_t longitude,
			uint8_t  num         =  10,
			uint16_t maxDistance = 500
		);
		DBdeparr* getStationBoard(
			const char type[4],
			const char* stationId,
			const char* target        = NULL,
			const char* Dtime         = NULL,
			const char* Ddate         = NULL,
			uint8_t     num           =    0,
			uint16_t    productFilter = 1023
		);
		DBdeparr* getDepatures(
			const char* stationId,
			const char* target        = NULL,
			const char* Dtime         = NULL,
			const char* Ddate         = NULL,
			uint8_t     num           =    0,
			uint16_t    productFilter = 1023
		);
		DBdeparr* getArrivals(
			const char* stationId,
			const char* target        = NULL,
			const char* Dtime         = NULL,
			const char* Ddate         = NULL,
			uint8_t     num           =    0,
			uint16_t    productFilter = 1023
		);
		
	
};

#endif //DBAPI_H