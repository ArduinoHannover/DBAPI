#include "DBAPI.h"
#include <ArduinoJson.h>
#include <Hash.h>
#ifdef ESP8266
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <new>
#include <StackThunk.h>
#include <AddrList.h>
#else
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lwip/netdb.h>
#endif

#ifdef DEBUG_ESP_PORT
#define DB_DEBUG_MSG(...) DEBUG_ESP_PORT.printf( __VA_ARGS__ )
#else
#define DB_DEBUG_MSG(...)
#endif

// Responses of the API take over a second at times, the Stream default is 1 s
#define DB_TIMEOUT 10000

#define DB_USER_AGENT "DBNavigator/iOS/26.12.1"

#ifndef ESP8266
#define DB_SET_CIPHERS(c) dbSetCiphers((c), dbCipherList, sizeof(dbCipherList) / sizeof(dbCipherList[0]), 0)
#endif

#ifdef ESP8266
// Emulate Apple ciphers to combat Akamai filter
static const uint16_t dbCipherList[] = {
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
};
#else
static const int dbCipherList[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    0
};
#endif

#ifndef ESP8266
template <class C>
static auto dbSetCiphers(C& c, const int* list, size_t len, int) -> decltype(c.setCiphers(list, len), void()) {
	c.setCiphers(list, len);
}
template <class C>
static void dbSetCiphers(C& c, const int* list, size_t, long) {
	c.setCiphers(list);
}
#endif

#ifdef ESP8266
/**
 * Minimal BearSSL client without certificate validation.
 * The core's WiFiClientSecure always sends the max_fragment_length extension
 * (its buffers are smaller than a full TLS record), which the Akamai filter blocks.
 * Buffers here hold a full record instead, so the extension is left out.
 * The engine is driven through the core's stack thunks, as the crypto needs more
 * stack than the sketch has.
 */
class DBTLS : public Stream {
	struct X509 {
		const br_x509_class* vt;
		br_x509_decoder_context dc;
		bool done;
	};
	struct Ctx {
		br_ssl_client_context sc;
		br_ec_impl ec;
		X509 x509;
	};
	static const size_t IN_LEN  = 16384 + 325;
	static const size_t OUT_LEN = 512 + 85;
	static const size_t THUNK_STACK = 6208; // the core allocates 6200 bytes for BearSSL
	// Kept between connections: with less than a few kB of heap left, new allocations
	// of this size fail once the heap is fragmented
	inline static Ctx* ctx = NULL;
	inline static uint8_t* ibuf = NULL;
	inline static uint8_t* obuf = NULL;
	inline static bool thunk = false;
	WiFiClient tcp;
	bool open = false;
	int peeked = -1;
	bool pending = false;

	// Accept any certificate, only the public key of the first one is needed
	static void xStart(const br_x509_class** c, const char*) {
		X509* x = (X509*)c;
		br_x509_decoder_init(&x->dc, NULL, NULL, NULL, NULL);
		x->done = false;
	}
	static void xStartCert(const br_x509_class**, uint32_t) {}
	static void xAppend(const br_x509_class** c, const unsigned char* b, size_t n) {
		X509* x = (X509*)c;
		if (!x->done) br_x509_decoder_push(&x->dc, b, n);
	}
	static void xEndCert(const br_x509_class** c) { ((X509*)c)->done = true; }
	static unsigned xEndChain(const br_x509_class** c) { return ((X509*)c)->done ? 0 : 1; }
	static const br_x509_pkey* xPkey(const br_x509_class* const* c, unsigned* u) {
		if (u) *u = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
		return &((X509*)c)->dc.pkey;
	}

	// Move records between engine and socket until one of the target states is reached
	bool run(unsigned target) {
		br_ssl_engine_context* e = &ctx->sc.eng;
		for (;;) {
			optimistic_yield(1000); // handshake crypto is slow, keep the watchdog happy
			unsigned st = br_ssl_engine_current_state(e);
			size_t n;
			if (st & BR_SSL_CLOSED) return false;
			if (st & BR_SSL_SENDREC) {
				unsigned char* b = BearSSL::thunk_br_ssl_engine_sendrec_buf(e, &n);
				size_t w = tcp.write(b, n);
				if (!w) return false;
				BearSSL::thunk_br_ssl_engine_sendrec_ack(e, w);
			} else if (st & target) {
				return true;
			} else if (st & BR_SSL_RECVREC) {
				unsigned char* b = BearSSL::thunk_br_ssl_engine_recvrec_buf(e, &n);
				for (uint32_t start = millis(); !tcp.available(); delay(1)) {
					if (!tcp.connected() || millis() - start > DB_TIMEOUT) return false;
				}
				int r = tcp.read(b, n);
				if (r <= 0) return false;
				BearSSL::thunk_br_ssl_engine_recvrec_ack(e, r);
			} else {
				br_ssl_engine_flush(e, 0);
			}
		}
	}
	int fetch() {
		sendPending();
		size_t n;
		if (!open || !run(BR_SSL_RECVAPP)) return -1;
		unsigned char* b = BearSSL::thunk_br_ssl_engine_recvapp_buf(&ctx->sc.eng, &n);
		int c = b[0];
		BearSSL::thunk_br_ssl_engine_recvapp_ack(&ctx->sc.eng, 1);
		return c;
	}
	void sendPending() {
		if (pending && open) {
			br_ssl_engine_flush(&ctx->sc.eng, 0);
			run(BR_SSL_SENDAPP | BR_SSL_RECVAPP);
		}
		pending = false;
	}

	// The API is only reliably reachable via IPv6 at times, so try that first if lwIP has it
	// (needs the PIO_FRAMEWORK_ARDUINO_LWIP2_IPV6_* build flag), then fall back to IPv4
	bool tcpConnect(const char* host, uint16_t port) {
		IPAddress ip;
#if LWIP_IPV4 && LWIP_IPV6
		static bool noV6 = false;
		if (!noV6 && !hasGlobalV6()) { // SLAAC takes a moment after connecting
			for (uint8_t i = 0; i < 50 && !hasGlobalV6(); i++) delay(100);
			noV6 = !hasGlobalV6();
		}
		if (!noV6 && WiFi.hostByName(host, ip, 5000, DNSResolveType::DNS_AddrType_IPv6) && tcp.connect(ip, port)) {
			DB_DEBUG_MSG("DBTLS: using IPv6 %s\n", ip.toString().c_str());
			return true;
		}
		DB_DEBUG_MSG("DBTLS: using IPv4\n");
		return WiFi.hostByName(host, ip, 5000, DNSResolveType::DNS_AddrType_IPv4) && tcp.connect(ip, port);
#else
		return tcp.connect(host, port);
#endif
	}
#if LWIP_IPV4 && LWIP_IPV6
	static bool hasGlobalV6() {
		for (auto a : addrList) {
			if (a.isV6() && !a.isLocal()) return true;
		}
		return false;
	}
#endif

public:
	~DBTLS() { stop(); }

	bool connect(const char* host, uint16_t port) {
		stop();
		if (!thunk) {
			// stack_thunk_add_ref() aborts if its stack doesn't fit, so check first and take it before the buffers
			void* probe = malloc(THUNK_STACK);
			if (!probe) return false;
			free(probe);
			stack_thunk_add_ref();
			thunk = true;
		}
		if (!ctx) ctx = new (std::nothrow) Ctx();
		if (!ibuf) ibuf = new (std::nothrow) uint8_t[IN_LEN];
		if (!obuf) obuf = new (std::nothrow) uint8_t[OUT_LEN];
		if (!ctx || !ibuf || !obuf || !tcpConnect(host, port)) {
			stop();
			return false;
		}
		open = true;
		tcp.setNoDelay(true);
		static const br_x509_class vt = {sizeof(X509), xStart, xStartCert, xAppend, xEndCert, xEndChain, xPkey};
		br_ssl_client_context* sc = &ctx->sc;
		br_ssl_engine_context* e = &sc->eng;
		br_ssl_client_zero(sc);
		br_ssl_engine_add_flags(e, BR_OPT_NO_RENEGOTIATION);
		br_ssl_engine_set_versions(e, BR_TLS10, BR_TLS12);
		br_ssl_engine_set_suites(e, dbCipherList, sizeof(dbCipherList) / sizeof(dbCipherList[0]));
		br_ssl_client_set_default_rsapub(sc);
		br_ssl_engine_set_default_rsavrfy(e);
		br_ssl_engine_set_default_ecdsa(e);
		br_ssl_engine_set_hash(e, br_sha256_ID, &br_sha256_vtable);
		br_ssl_engine_set_hash(e, br_sha384_ID, &br_sha384_vtable);
		br_ssl_engine_set_hash(e, br_sha512_ID, &br_sha512_vtable);
		br_ssl_engine_set_prf10(e, &br_tls10_prf);
		br_ssl_engine_set_prf_sha256(e, &br_tls12_sha256_prf);
		br_ssl_engine_set_prf_sha384(e, &br_tls12_sha384_prf);
		br_ssl_engine_set_default_aes_cbc(e);
		br_ssl_engine_set_default_aes_gcm(e);
		br_ssl_engine_set_default_chapol(e);
		// The filter blocks the fingerprint of the default hello, so deviate: only
		// P-256/X25519 (no SHA-1 above) and announce HTTP/1.1 via ALPN
		static const char* alpn[] = {"http/1.1"};
		ctx->ec = *br_ec_get_default();
		ctx->ec.supported_curves &= (1UL << BR_EC_secp256r1) | (1UL << BR_EC_curve25519);
		br_ssl_engine_set_ec(e, &ctx->ec);
		br_ssl_engine_set_protocol_names(e, alpn, 1);
		ctx->x509.vt = &vt;
		br_ssl_engine_set_x509(e, &ctx->x509.vt);
		br_ssl_engine_set_buffers_bidi(e, ibuf, IN_LEN, obuf, OUT_LEN);
		e->log_max_frag_len = 14; // 14 = default, do not announce max_fragment_length
		br_ssl_client_reset(sc, host, 0);
		if (!run(BR_SSL_SENDAPP | BR_SSL_RECVAPP)) { // runs the handshake
			DB_DEBUG_MSG("DBTLS: handshake failed, engine error %d\n", br_ssl_engine_last_error(e));
			stop();
			return false;
		}
		return true;
	}
	void stop() {
		tcp.stop();
		open = false;
		peeked = -1;
		pending = false;
	}
	size_t write(const uint8_t* b, size_t n) override {
		br_ssl_engine_context* e = &ctx->sc.eng;
		for (size_t done = 0; done < n;) {
			size_t len;
			if (!open || !run(BR_SSL_SENDAPP)) return done;
			unsigned char* buf = BearSSL::thunk_br_ssl_engine_sendapp_buf(e, &len);
			if (len > n - done) len = n - done;
			memcpy(buf, b + done, len);
			BearSSL::thunk_br_ssl_engine_sendapp_ack(e, len);
			done += len;
			pending = true;
		}
		return n;
	}
	size_t write(uint8_t c) override { return write(&c, 1); }
	void flush() override { sendPending(); }
	int available() override {
		size_t n = 0;
		if (open) BearSSL::thunk_br_ssl_engine_recvapp_buf(&ctx->sc.eng, &n);
		return n + (peeked >= 0);
	}
	int read() override {
		int c = peeked >= 0 ? peeked : fetch();
		peeked = -1;
		return c;
	}
	int peek() override {
		if (peeked < 0) peeked = fetch();
		return peeked;
	}
};
#define DB_CLIENT(c) DBTLS c; (c).setTimeout(DB_TIMEOUT)
#define DB_CONNECT(c) (c).connect(host, 443)
#else
#define DB_CLIENT(c) WiFiClientSecure c; DB_SET_CIPHERS(c); c.setInsecure(); c.setTimeout(DB_TIMEOUT) // Don't check fingerprint
#define DB_CONNECT(c) dbConnect(c, host)

/**
 * The API is only reliably reachable via IPv6 at times. With WiFi.enableIPv6() called before
 * connecting, the core prefers IPv6 once the network provided a global address, but it has no
 * IPv4 fallback if that fails, so retry with the IPv4 address here (SNI needs the hostname).
 */
static bool dbConnect(WiFiClientSecure& c, const char* host) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
	static bool noV6 = false;
	if (!noV6 && !WiFi.STA.hasGlobalIPv6()) {
		// SLAAC takes a moment after connecting, no link local address within 1 s means IPv6 is off
		for (uint8_t i = 0; i < 50 && !WiFi.STA.hasGlobalIPv6() && (i < 10 || WiFi.STA.hasLinkLocalIPv6()); i++) delay(100);
		noV6 = !WiFi.STA.hasGlobalIPv6();
	}
	if (c.connect(host, 443)) {
		DB_DEBUG_MSG("DBAPI: connected to %s\n", c.remoteIP().toString().c_str());
		return true;
	}
	c.stop();
	struct addrinfo hints = {};
	struct addrinfo* res;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (lwip_getaddrinfo(host, "443", &hints, &res) != 0) return false;
	IPAddress ip(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
	lwip_freeaddrinfo(res);
	DB_DEBUG_MSG("DBAPI: falling back to IPv4 %s\n", ip.toString().c_str());
	return c.connect(ip, 443, host, NULL, NULL, NULL);
#else
	return c.connect(host, 443);
#endif
}
#endif

DBAPI::DBAPI() {

}

/**
 * Get stations matching the name or close to an address
 * @param name    Name of the station to get
 * @param address Address to request nearest stations from - currently not implemented, leave as NULL
 * @param num     Maxmium of stations to request
 * @return DBstation* array, possibly NULL if no results were found
 * @note see getError to check if request was successful and is just empty because no station was found
 */
DBstation* DBAPI::getStation(
		const char* name,
		const char* address,
		uint8_t     num
	) {
	// Assume all is good at first
	err = DBERR_NONE;
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
	DB_CLIENT(client);
	if (!DB_CONNECT(client)) {
		DB_DEBUG_MSG("DBAPI: Connection to Host failed.\n");
		err = DBERR_REQ_FAILED;
		return NULL;
	}
	String json = String("{\"locationTypes\":[\"ST\"],\"searchTerm\":\"") + name + "\"}";
	client.print(String("POST /mob/location/search HTTP/1.1\r\n") +
		"Host: " + host + "\r\n" +
		"User-Agent: " DB_USER_AGENT "\r\n" +
		"X-Correlation-ID: FOO\r\n" +
		"Content-Type: application/x.db.vendo.mob.location.v3+json\r\n" +
		"Accept: application/x.db.vendo.mob.location.v3+json\r\n" +
		"Content-Length: " + json.length() + "\r\n\r\n" +
		json
		);
	
	if (checkStatus(client) / 100 != 2) {
		err = DBERR_RESP_INVALID;
		return stations;
	}
	char endOfHeaders[] = "\r\n\r\n";
	if (!client.find(endOfHeaders)) {
		DB_DEBUG_MSG("Did not find headers\n");
		err = DBERR_RESP_INVALID;
		return stations;
	}
	JsonDocument doc;
	DeserializationError error = deserializeJson(doc, client);
	if (error) {
		DB_DEBUG_MSG("deserializeJson() on Station failed");
		DB_DEBUG_MSG("%s\n", error.c_str());
		err = DBERR_DESERIALIZATION_FAILED;
		return stations;
	}
	DBstation* prev = NULL;
	for (uint8_t i = 0; i < doc.size() && i < num; i++) {
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
 * Get last errorcode to check if the request was successful.
 * Most likely to be useful, when NULL is returned, but it can't be distinguished,
 * if the request was malformed, any function or request failed or there are simply
 * no more results available with the given parameters.
 * @return DBerror enum errorcode
 */
DBerror DBAPI::getError() {
	return err;
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
 * @note see getError to check if request was successful and is just empty because no service is available
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
	// Assume all is good at first
	err = DBERR_NONE;
	// sanity check, if no station is supplied, a request would crash ArduinJSON later.
	if (stationId == NULL || !strlen(stationId)) {
		DB_DEBUG_MSG("DBAPI: StationID was not supplied.\n");
		err = DBERR_NOSTATIONID;
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
		DB_CLIENT(client);
		// The API sometimes drops the connection or answers with 5xx, so try a few times.
		// 4xx like 429 (rate limit) is not retried, that would only make it worse.
		int code = -1;
		for (uint8_t attempt = 0; attempt < 3; attempt++) {
			if (attempt) {
				client.stop();
				delay(500 * attempt);
			}
			if (!DB_CONNECT(client)) {
				DB_DEBUG_MSG("DBAPI: Connection to Host failed.\n");
				code = -1;
				continue;
			}
			DB_DEBUG_MSG("DBAPI: Requesting StationBoard.\n");
			DB_DEBUG_MSG("%s\n", output);
			client.print(String("POST /mob/bahnhofstafel/") + type + " HTTP/1.1\r\n" +
				"Host: " + host + "\r\n" +
				"User-Agent: " DB_USER_AGENT "\r\n" +
				"X-Correlation-ID: FOO\r\n" +
				"Content-Type: application/x.db.vendo.mob.bahnhofstafeln.v2+json\r\n" +
				"Accept: application/x.db.vendo.mob.bahnhofstafeln.v2+json\r\n" +
				"Content-Length: " + strlen(output) + "\r\n\r\n" + output);
			code = checkStatus(client);
			if (code / 100 == 2 || (code > 0 && code < 500)) break;
		}
		free(output);
		if (code / 100 != 2) {
			err = code < 0 ? DBERR_REQ_FAILED : DBERR_RESP_INVALID;
			// Return so far accumulated array
			return deparr;
		}
		char endOfHeaders[] = "\r\n\r\n";
		if (!client.find(endOfHeaders)) {
			DB_DEBUG_MSG("DBAPI: Did not find headers\n");
			err = DBERR_RESP_INVALID;
			// Return so far accumulated array
			return deparr;
		}
		JsonDocument doc;
		if (!client.find(":[")) { // Skip to first element
			err = DBERR_NO_JSON_FOUND;
			return deparr;
		}
		do {
			DeserializationError error = deserializeJson(doc, client);
			if (error) {
				DB_DEBUG_MSG("DBAPI: deserializeJson() on departures/arrivals failed");
				DB_DEBUG_MSG("%s\n", error.c_str());
				//err = DBERR_DESERIALIZATION_FAILED;
				//return deparr;
				// No data in array, continue with next hour if selected
				break;
			}

			uint8_t current_hash[20];
			// hash to save RAM as the ID is >150 chars
			sha1(doc["zuglaufId"].as<String>(), current_hash);

			bool match = false;
			for (uint8_t i = 0; i < cnt && !match; i++) {
				match |= !memcmp(hashes[i], current_hash, sizeof(current_hash));
			}
			// duplicate entry found, skipping
			if (match) continue;
			memcpy(hashes[cnt], current_hash, sizeof(current_hash));

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
			da->missingCars = false;
			da->highLoad = false;
			for (uint8_t i = 0; i < arr.size(); i++) {
				String desc = arr[i]["text"].as<String>();
				if (desc.equals("Halt entfällt")) {
					da->cancelled = true;
				} else if (desc.indexOf("Wagen fehl") >= 0) {
					da->missingCars = true;
				} else if (desc.indexOf("Hohe Auslastung") >= 0) {
					da->highLoad = true;
				} else {
					DB_DEBUG_MSG("%s\n", desc.c_str());
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
 * Reads the HTTP status line
 * @return HTTP status code, 0 if there was no status line
 */
int DBAPI::checkStatus(Stream& client) {
	// "HTTP/1.1 200 OK"
	if (!client.find("HTTP/1.1 ")) {
		DB_DEBUG_MSG("DBAPI: No status line\n");
		return 0;
	}
	int code = client.parseInt();
	if (code < 200 || code > 299) {
		DB_DEBUG_MSG("DBAPI: HTTP status %d\n", code);
#ifdef DEBUG_ESP_PORT
		if (client.find("\r\n\r\n")) {
			DEBUG_ESP_PORT.println(client.readString());
		}
#endif
	}
	return code;
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