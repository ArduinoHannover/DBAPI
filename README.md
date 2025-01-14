# DBAPI
Hacon/Hafas/Deutsche Bahn API für ESP8266

![tft_hannover_hbf](https://user-images.githubusercontent.com/193273/200298925-0f80dfdb-e17f-4f26-a28f-67b808540332.jpg)
Abfartstafel aus dem DBTFT Beispiel

## Abfrage

### Stationen
Für weitere Anfragen wird die Stations-ID benötigt.
Daher muss i.d.R. erst einmal diese abgefragt werden (kann dann statisch im Code hinterlegt werden).

`DBstation* getStation(name, address, num)`

| Variable | Typ | Standard | Funktion |
| --- | --- | --- | --- |
| `name` | `const char*` | `NULL` | Entspricht dem Stationsnamen, hier können Variationen den Unterschied machen. Während `Hannover Hbf` den DB Bahnhof ausgibt, kommen bei `Hauptbahnhof Hannover` hingegen die Haltestellen des Nahverkehrs (Bus/Bahn). |
| `address` | `const char*` | `NULL` |  Kann alternativ angegeben werden (`name` auf `NULL` setzen), um die nächstgelegene Station zu einer Anschrift zu finden. |
| `num` | `uint8_t` | `10` | Begrenzt die Anzahl der ausgegebenen Stationen |

### Ankunft/Abfahrt

`DBdeparr* getStationBoard(type, stationID, target, Dtime, Ddate, num, productFilter)`

`DBdeparr* getDepatures(stationID, target, Dtime, Ddate, num, productFilter)`

`DBdeparr* getArrivals(stationID, target, Dtime, Ddate, num, productFilter)`

`getDepatures` und `getArrivals` verweisen nur auf `getStationBoard` mit dem entsprechenden `type`.

| Variable | Typ | Standard | Funktion |
| --- | --- | --- | --- |
| `type` | `char[4]` | | Kann entweder `dep` (Abfahrt) oder `arr` (Ankunft) sein. |
| `stationID` | `char[11]` | | ID aus der `DBstation` |
| `target` | `const char*` | `NULL` | entfallen |
| `Dtime` | `const char*` | `NULL` | Zeit in `HH:MM` oder `NULL`/`actual` für "jetzt" |
| `Ddate` | `const char*` | `NULL` | Datum in `yyyy-mm-dd` oder `NULL` für "heute" |
| `num` | `uint8_t` | `10` | Anzahl der Ergebnisse |
| `productFilter` | `uint16_t` | `1023`/alle | Verkehrsmittel, die angezeigt werden sollen |

#### Produktfilter

Folgende Filter sind möglich:

* `PROD_ICE`
* `PROD_IC_EC`
* `PROD_IR`
* `PROD_RE`
* `PROD_S`
* `PROD_BUS`
* `PROD_SHIP`
* `PROD_U`
* `PROD_STB`
* `PROD_AST` (Anruf-Sammel-Taxi)

Mittels bitweiser Oder-Verknüpfung (`|`) können mehrere Verkehrsmittel angefragt werden.

## Datenstrukturen

### `DBstation`
| Name | Typ | Funktion |
| --- | --- | --- |
| `name` | `char[30]` | Stationsname |
| `stationId` | `char[50]` | Stations ID |
| `longitude` | `float` | Längengrad |
| `latitude` |  `float` | Breitengrad |
| `next` | `DBstation*` | Nächste Station in der Liste |

### `DBdeparr`

| Name | Typ | Funktion |
| --- | --- | --- |
| `time` | `time_t` | Reguläre Abfahrtszeit als Unix-Timestamp |
| `realTime` | `time_t` | Tatsächliche Abfahrtszeit als Unix-Timestamp |
| `delay` | `int16_t` | Verspätung |
| `cancelled` | `bool` | Zug entfällt |
| `platform` | `char[8]` | Geplantes Gleis |
| `newPlatform` | `char[8]` | Tatsächliches Gleis, sofern geändert, sonst leer |
| `target` | `char[50]` | Zielhaltestelle |
| `product` | `char[12]` | Bezeichnung des Verkehrsmittels |
| `line` | `uint16_t` | Liniennummer |
| `textline` | char[20] | Liniennummer als String (kann z.B. auch `SEV` sein) |
| `next` | `DBdeparr*` | Nächste Fahrt in Liste |

### Hinweise zur Datenstruktur

Wird eine neue Abfrage gestartet, so werden die zuletzt angefragten Daten überschrieben.
Nutzdaten müssen vorher ggf. gespeichert werden.
Werden etwa Ankunft *und* Abfahrt eines Bahnhofs angefragt, so ist nur das `DBdeparr*` der letzten Anfrage gültig.

Mittels `for (; t != NULL; t = t->next) {...}` kann über die Stationen bzw. Fahrten iteriert werden (`t` vom Typ einer der beiden obigen Datenstrukturen).
Soll die Referenz auf den Anfang nicht verloren gehen, so muss `t` mittels `Type* t = orig_t;` erstellt werden.

## Rechtliche Hinweise
Die Bibliothek fragt direkt die Deutsche Bahn Schnittstelle an.
Für die korrekte Funktion der Schnittstelle bzw. deren Daten kann keine Garantie übernommen werden.
Die Benutzung erfolgt auf eigene Gefahr.
