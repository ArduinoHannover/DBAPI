#ifndef DBAPI_H
#define DBAPI_H

#include <Arduino.h>
#include <TimeLib.h>

struct DBdeparr {
	time_t    time;
	time_t    realTime;
	int16_t   delay;
	bool      cancelled;
	char      platform[8];
	char      newPlatform[8]; 
	char      target[50];
	char      product[12];
	uint16_t  line;
	char      textline[20]; //Apperently there are services like Bus 5/43, so no numeric value
	DBdeparr* next;
};

struct DBstation {
	char       name[30];
	char       stationId[150];
	float      longitude;
	float      latitude;
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
		const char* host = "app.vendo.noncd.db.de";
		time_t parseTime(String t);
		DBdeparr*  deparr   = NULL;
		DBstation* stations = NULL;
		bool agfx           = false;
		static const char* services[];
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
		DBdeparr* getDepartures(
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
		// Output Adafruit GFX compatible Umlauts for default font
		void setAGFXOutput(bool gfx);
};

#endif //DBAPI_H