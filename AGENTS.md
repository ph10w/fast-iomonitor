# Fast IoMonitor – Projektanweisungen für Codex

## Geltungsbereich

Diese Datei gilt für das gesamte Repository. Fast IoMonitor ist ein
Windows-Dateisystem-Minifilter, der Lese- und Schreibzugriffe ausgewählter
Prozesse erfasst und als UTF-8-CSV ausgibt.

## Architektur und stabile Schnittstellen

- `driver/` enthält `IoMonitor.sys`, den Kernel-Minifilter.
- `service/` enthält `IoMonitorService.exe`, einen bei Bedarf vom Client
  gestarteten `LocalSystem`-Broker zwischen Treiber und normalem Benutzerclient.
- `client/` enthält `IoMonitorClient.exe`, der ohne Administratorrechte läuft.
- `shared/io_monitor_protocol.h` definiert das feste Nachrichtenformat für
  Treiber, Dienst und Client. Die aktuelle Protokollversion ist 5.
- Der sichtbare Projektname ist **Fast IoMonitor**. Interne Laufzeitkennungen wie
  `IoMonitor`, `IoMonitorService`, `IoMonitor.sys`, Filter-Port und Named Pipe
  bleiben aus Kompatibilitätsgründen bestehen. Nicht ohne ausdrücklichen
  Migrationsauftrag umbenennen.
- Wenn sich Layout oder Semantik einer Protokollstruktur inkompatibel ändern,
  `IO_MONITOR_PROTOCOL_VERSION` erhöhen und Treiber, Dienst und Client gemeinsam
  anpassen.

## Gewünschtes Laufzeitverhalten

- Der Client akzeptiert `--process-name` oder alternativ `--pid`.
- Sobald die Ziel-PIDs bekannt sind, startet der Client den Broker-Dienst und
  beendet ihn nach dem Leeren der Event-Queue beim eigenen Abschluss wieder.
- Ist bei `--process-name` noch kein Prozess vorhanden, wartet der Client und
  sucht alle 500 ms. `Strg+C` muss auch diese Wartephase beenden.
- Nach dem ersten Treffer werden die zu diesem Zeitpunkt gefundenen PIDs
  überwacht. Später gestartete gleichnamige Prozesse werden derzeit nicht
  ergänzt. Der Client endet, sobald alle überwachten Prozesse beendet sind.
- `--operation Read|Write|All` wird bereits im Treiber gefiltert. Ohne Parameter
  gilt `All`.
- Kann der Treiber einen Dateinamen in einem zulässigen I/O-Kontext nicht sofort
  ermitteln, wird das Ereignis verworfen. Es darf später weder aufgelöst noch mit
  leerem Pfad protokolliert werden.
- Die CSV enthält genau diese Spalten:
  `timestamp_utc,process_image,operation,success,path`.
- Bekannte NT-Gerätepfade werden im Client über eine beim Start gecachte
  Laufwerkszuordnung in DOS-/Laufwerkspfade umgewandelt. Unbekannte Pfade bleiben
  unverändert.
- Der Minifilter beobachtet nur. Er darf Dateioperationen weder blockieren noch
  Daten oder Rückgabestatus verändern.

## Performance- und Kernel-Invarianten

- Ziel-PID und Operation im Pre-Operation-Callback prüfen, bevor Speicher
  reserviert oder ein Dateiname abgefragt wird.
- Den lock-freundlichen Ziel-Snapshot und den negativen PID-Bloom-Vorfilter im
  Hot Path erhalten. Keine pro-I/O-Listenallokationen oder linearen
  User-Mode-Roundtrips einführen.
- Dateinamen pro Stream-Handle cachen und bei Umbenennung beziehungsweise Cleanup
  korrekt verwerfen.
- Ereignisse über die begrenzte Nonpaged-Queue übertragen: Kapazität 1024,
  Batches mit höchstens 32 Ereignissen.
- Ereignisobjekte weiterhin aus der `NPAGED_LOOKASIDE_LIST` beziehen.
- Das Queue-Event nur beim Übergang von leer zu nicht leer setzen.
- Den günstigen Kernel-Zeitstempelpfad mit `KeQuerySystemTime` beibehalten, sofern
  keine ausdrücklich höhere Genauigkeit gefordert wird.
- IRQL-, Nonpaged-Memory-, Synchronisations- und Minifilter-Regeln beachten.
  Keine blockierenden oder pageable Operationen in ungeeignete Callback-Kontexte
  verschieben.
- Bei voller Queue oder fehlgeschlagener Allokation das Ereignis verwerfen und
  `DroppedEvents` erhöhen; niemals den überwachten I/O dafür verzögern.

## Plattform und Signierung

- Zielplattform ist Windows 10 ab Version 2004 (Build 19041) oder Windows 11,
  ausschließlich x64.
- `_NT_TARGET_VERSION` bleibt auf `0xA000008`, damit der Treiber keine auf der
  Mindestversion fehlenden Kernelimporte erzeugt.
- Lokale Builds sind testsigniert. Installation und Laden setzen eine passende
  Testumgebung mit aktiviertem Windows-Testmodus voraus.
- Die Altitude `385201` ist nur ein Testwert. Vor einer Verteilung muss eine
  offizielle Minifilter-Altitude zugewiesen und der Treiber produktionssigniert
  werden.

## Build und Verifikation

- Standardkonfiguration ist `Release`.
- Vollständiger Build aus einer normalen PowerShell:

  ```powershell
  pwsh.exe -File .\build.ps1
  ```

- Nur den Client bauen:

  ```powershell
  pwsh.exe -File .\build.ps1 -ClientOnly
  ```

- Der vollständige Build benötigt Visual Studio 2022, Windows SDK und WDK. Er
  muss mit 0 Fehlern und 0 Warnungen abschließen; insbesondere müssen
  ApiValidator und Inf2Cat erfolgreich sein.
- Nach Änderungen an PowerShell-Skripten beide Dateien zusätzlich mit dem
  PowerShell-Parser auf Syntaxfehler prüfen.
- Installation, Laden, Entladen und Deinstallation verändern den Systemzustand
  und benötigen erhöhte Rechte. Diese Schritte nur auf ausdrücklichen Auftrag
  ausführen.
- Unterstützte Administratoraufrufe:

  ```powershell
  sudo pwsh.exe -File .\install-service.ps1 -LoadDriver
  sudo pwsh.exe -File .\install-service.ps1 -Uninstall
  ```

- Nach Treiber- oder Protokolländerungen mindestens einen vollständigen
  Release-Build ausführen. Einen Laufzeittest gegen den installierten Dienst nur
  in einer ausdrücklich freigegebenen Testumgebung durchführen.

## Dokumentation und Änderungen

- README-Aufrufe ausschließlich für `Release` dokumentieren. Da `Release` der
  Standard ist, `-Configuration Release` in Beispielen nicht angeben.
- Pro Abschnitt möglichst nur einen repräsentativen Aufruf zeigen und die
  Parameter danach einzeln beschreiben.
- Bestehende Benutzeränderungen und nicht zum Auftrag gehörende Dateien erhalten.
- Keine destruktiven Git-Befehle verwenden. Änderungen eng am Auftrag halten und
  relevante Builds oder Prüfungen vor der Übergabe ausführen.
