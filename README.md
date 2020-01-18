# DBAPI
Hacon/Hafas/Deutsche Bahn API für ESP8266

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
| `target` | `const char*` | `NULL` | Ziel oder Zwischenhalt (nur in eine Richtung Fahrten erhalten) |
| `Dtime` | `const char*` | `NULL` | Zeit in `HH:MM` oder `NULL`/`actual` für "jetzt" |
| `Ddate` | `const char*` | `NULL` | Datum in `dd.mm.yy` oder `NULL` für "heute" |
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
| `stationId` | `char[11]` | Stations ID |
| `longitude` | `uint32_t` | Längengrad (x 1000000) |
| `latitude` |  `uint32_t` | Breitengrad (x 1000000) |
| `next` | `DBstation*` | Nächste Station in der Liste |

### `DBdeparr`

| Name | Typ | Funktion |
| --- | --- | --- |
| `time` | `char[6]` | Zeit im Format `HH:MM` |
| `date` | `char[9]` | Datum im Format `dd.mm.yy` |
| `textdelay` | `char[5]` | Verspätung als Text (`-` oder `+xxx`; je nach Verfügbarkeit im Verkehrsverbund) |
| `delay` | `uint16_t` | Verspätung als Zahl |
| `platform` | `char[5]` | Gleis |
| `target` | `char[50]` | Zielhaltestelle |
| `product` | `char[5]` | Bezeichnung des Verkehrsmittels |
| `line` | `uint16_t` | Liniennummer |
| `next` | `DBdeparr*` | Nächste Fahrt in Liste |

### Hinweise zur Datenstruktur

Wird eine neue Abfrage gestartet, so werden die zuletzt angefragten Daten überschrieben.
Nutzdaten müssen vorher ggf. gespeichert werden.
Werden etwa Ankunft *und* Abfahrt eines Bahnhofs angefragt, so ist nur das `DBdeparr*` der letzten Anfrage gültig.

Mittels `for (; t != NULL; t = t->next) {...}` kann über die Stationen bzw. Fahrten iteriert werden (`t` vom Typ einer der beiden obigen Datenstrukturen).
Soll die Referenz auf den Anfang nicht verloren gehen, so muss `t` mittels `Type* t = orig_t;` erstellt werden.

Verspätungen über 999 Minuten können nicht in Textform ausgegeben werden.

## Rechtliche Hinweise
Die Bibliothek fragt direkt die Deutsche Bahn Schnittstelle an.
Für die korrekte Funktion der Schnittstelle bzw. deren Daten kann keine Garantie übernommen werden.
Die Benutzung erfolgt auf eigene Gefahr.
