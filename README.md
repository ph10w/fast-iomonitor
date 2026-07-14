# Fast IoMonitor

Fast IoMonitor protokolliert Datei-Lese- und -Schreibzugriffe ausgewählter Prozesse
unter Windows in eine UTF-8-CSV. Ein privilegierter Dienst kommuniziert mit dem
Minifilter; der Client läuft ohne Administratorrechte.

> **Wichtig:** Dies ist ein Lern- und Test-MVP für eine isolierte Test-VM, kein
> produktionsreifer Treiber. Ein Fehler in Kernel-Code kann Windows zum Absturz
> bringen. Für eine Verteilung sind unter anderem ein zugewiesener Minifilter-
> Altitude-Wert, Produktionssignierung, Lasttests und eine Sicherheitsprüfung nötig.

## Aufbau

```text
Zielprozesse
  -> IRP_MJ_READ / IRP_MJ_WRITE
  -> IoMonitor.sys (Handle-Pfadcache und Post-Operation-Status)
  -> begrenzte Nonpaged-Queue (1024 Ereignisse)
  -> Batches mit bis zu 32 Ereignissen
  -> IoMonitorService.exe (LocalSystem)
  -> abgesicherte lokale Named Pipe
  -> IoMonitorClient.exe
  -> UTF-8-CSV
```

Der Treiber blockiert keine Dateioperation und verändert weder Daten noch Status.
Ziel-PID und gewählte Operation werden im Pre-Operation-Callback geprüft, bevor
Speicher reserviert oder der Dateiname aufgelöst wird. Normalisierte Pfade werden
nach der ersten erfolgreichen Abfrage pro Stream-Handle im Treiber
zwischengespeichert. Ist die Queue voll oder schlägt eine
Nonpaged-Pool-Allokation fehl, verwirft der Treiber das Ereignis und erhöht
`DroppedEvents`. Der Client meldet diese Verluste auf `stderr`.

Verzeichnisse:

- `driver/`: Minifilter, INF und WDK-Projekt
- `client/`: C++-Konsolenlogger
- `service/`: privilegierter Windows-Dienst und Named-Pipe-Broker
- `shared/`: festes Nachrichtenformat zwischen Kernel und User Mode
- `FastIoMonitor.sln`: Visual-Studio-Lösung für x64 Debug/Release
- `build.ps1`: Build-Helfer; `-ClientOnly` funktioniert ohne WDK
- `install-service.ps1`: installiert optional den Treiber und installiert oder
  entfernt `IoMonitorService`

Die internen Laufzeitkennungen und Binärnamen beginnen aus Kompatibilitätsgründen
weiterhin mit `IoMonitor`. So aktualisiert die umbenannte Version eine vorhandene
Installation, statt einen zweiten Treiber und Dienst parallel anzulegen.

## Voraussetzungen

- Windows 10 ab Version 2004 (Build 19041) oder Windows 11 x64
- Visual Studio mit **Desktop development with C++**
- zum Treiberbau zusätzlich das zur SDK-/VS-Version passende **Windows Driver Kit
  (WDK)** mit Visual-Studio-Integration
- administrative Rechte zum Installieren und Laden des Minifilters sowie zum
  Installieren des Windows-Dienstes; der Aufzeichnungsclient benötigt sie nicht
- für den Release-Treiber eine entsprechend konfigurierte Testsignierungsumgebung

Das WDK stellt `fltKernel.h`, `FltMgr.lib`, INF-Prüfung und Treibersignierung bereit.
Der normale Windows SDK reicht nur für den User-Mode-Client.

## Bauen

Der vollständige Build wird in einer normalen PowerShell im Projektverzeichnis mit
genau diesem Aufruf gestartet:

```powershell
pwsh.exe -File .\build.ps1
```

Bedeutung des Aufrufs und seiner Parameter:

- `pwsh.exe` startet PowerShell 7.
- `-File .\build.ps1` führt das Buildskript aus dem aktuellen Verzeichnis aus.
- `-Configuration <Debug|Release>` wählt optional die Buildkonfiguration. Ohne den
  Parameter wird `Release` gebaut.
- `-ClientOnly` baut ausschließlich `IoMonitorClient.exe` und benötigt deshalb
  kein WDK.
- `-VisualStudio 2022` verwendet Visual Studio 2022. Alternativ wählt
  `-VisualStudio Latest` die neueste gefundene Visual-Studio-Installation; der
  Standardwert ist `2022`.

Die Ergebnisse landen in `bin\x64\Debug` beziehungsweise `bin\x64\Release`.
Neben Treiber und Client erzeugt der vollständige Build dort auch
`IoMonitorService.exe`. `build.ps1` verändert weder Dienste noch Zertifikatspeicher
und benötigt deshalb keine Administratorrechte.

## Testsignierung

64-Bit-Windows lädt keinen unsignierten Kernel-Treiber. Der genaue Ablauf hängt
von der lokalen WDK-/Zertifikatskonfiguration ab. Für eine wegwerfbare Test-VM kann
der von Visual Studio test-signierte Release-Treiber mit aktiviertem Windows-
Testmodus verwendet werden. Das Aktivieren des Testmodus verändert die
Boot-Sicherheitskonfiguration und erfordert einen Neustart; bei aktivem Secure Boot
kann es abgewiesen werden.

Nicht auf einem Produktivsystem ausführen. Die Aktivierung erfolgt in einer
administrativen Konsole typischerweise mit:

```powershell
bcdedit /set testsigning on
```

Nach dem Test kann der Modus wieder deaktiviert werden:

```powershell
bcdedit /set testsigning off
```

Beide Änderungen werden erst nach einem Neustart wirksam.

Die INF verwendet `SERVICE_DEMAND_START`; der Treiber startet also nicht automatisch
mit Windows. Für Windows 10 und Windows 11 vor 24H2 trägt sie die Instanzwerte unter
`Instances` ein; für Windows 11 ab 24H2 zusätzlich unter `Parameters\Instances`.

## Treiber und Dienst installieren

Nach dem Build werden Treiber und Broker-Dienst gemeinsam aus einer
administrativen PowerShell installiert und gestartet:

```powershell
sudo pwsh.exe -File .\install-service.ps1 -LoadDriver
```

Bedeutung des Aufrufs und der Parameter:

- `sudo` erhöht ausschließlich das Installationsskript. Der spätere
  `IoMonitorClient.exe` läuft ohne Erhöhung.
- `pwsh.exe -File .\install-service.ps1` führt die Treiber- und
  Dienstinstallation aus.
- `-Configuration <Debug|Release>` wählt optional Treiberpaket und Dienstdatei.
  Ohne den Parameter wird `bin\x64\Release` verwendet.
- `-LoadDriver` installiert das Treiberpaket und lädt den Minifilter. Ist er bereits
  geladen, wird er nach dem Stoppen des Brokers entladen und neu geladen.
  `-LoadDriver` schließt `-InstallDriver` ein.
- `-InstallDriver` installiert das Treiberpaket, erzwingt aber keinen Reload eines
  bereits laufenden Minifilters. Ist er noch nicht geladen, wird er beim Start des
  abhängigen Broker-Dienstes geladen.
- Ohne `-InstallDriver` oder `-LoadDriver` wird ausschließlich der Broker installiert
  beziehungsweise aktualisiert; der Treiber muss dann bereits registriert sein.
- `-Uninstall` stoppt und entfernt den Broker, entlädt den Minifilter und löscht
  alle eindeutig als IoMonitor erkannten OEM-Treiberpakete aus dem Driver Store.
  Das möglicherweise mit anderen Testtreibern gemeinsam verwendete
  WDK-Testzertifikat bleibt bestehen. Dieser Parameter darf nicht mit den
  Treiberparametern kombiniert werden.

Beim Installieren des Treibers prüft das Skript das WDK-Testzertifikat und
importiert es bei Bedarf in `LocalMachine\Root` und
`LocalMachine\TrustedPublisher`. Dies darf ausschließlich auf einem Testsystem
geschehen. Der zuletzt importierte Thumbprint wird in
`.iomonitor-cert-state.json` festgehalten. Wechselt das WDK-Zertifikat, entfernt
das Skript nur das zuvor für IoMonitor vermerkte Zertifikat; andere
WDK-Zertifikate bleiben unangetastet.

Das Skript kopiert den Dienst nach `%ProgramFiles%\IoMonitor`, registriert ihn als
automatisch gestarteten `LocalSystem`-Dienst und trägt den Minifilter `IoMonitor`
als Abhängigkeit ein. Bei einem Neustart lässt der Service Control Manager deshalb
zuerst den Minifilter und anschließend den Broker starten.

Normale Benutzer erhalten bewusst keine Rechte zum Starten oder Stoppen des
Dienstes. Das ist für die Aufzeichnung nicht erforderlich, weil der Dienst
automatisch läuft. Seine Named Pipe akzeptiert ausschließlich lokale,
authentifizierte Benutzer. Vor dem Setzen der Ziel-PIDs prüft der Dienst außerdem,
dass alle Zielprozesse demselben Windows-Benutzer wie der verbundene Client
gehören.

## Aufzeichnen

Alle aktuell laufenden Prozesse mit einem EXE-Namen werden mit diesem Aufruf ohne
Administratorrechte überwacht:

```powershell
.\bin\x64\Release\IoMonitorClient.exe --process-name notepad.exe --operation Read --output .\io_access.csv
```

Bedeutung des Aufrufs und der verfügbaren Parameter:

- `.\bin\x64\Release\IoMonitorClient.exe` startet den zuvor gebauten
  Release-Client.
- `--process-name notepad.exe` ermittelt alle laufenden Prozesse mit diesem
  Basisnamen. Ist beim Start noch kein passender Prozess vorhanden, wartet der
  Client auf den ersten Treffer. Der Vergleich ignoriert Groß- und Kleinschreibung;
  `.exe` darf weggelassen werden.
- `--operation Read` lässt bereits der Minifilter ausschließlich Lesezugriffe
  erfassen. Zulässig sind `Read`, `Write` und `All`; Groß- und Kleinschreibung
  werden ignoriert. Ohne den Parameter gilt `All`.
- `--output .\io_access.csv` bestimmt die Ausgabedatei. Ohne diesen Parameter wird
  `io_access.csv` im aktuellen Verzeichnis verwendet.
- `--append` ergänzt eine vorhandene CSV. Ohne diesen Parameter wird die Datei beim
  Start neu erstellt beziehungsweise überschrieben.
- `--pid <PID>` überwacht alternativ genau eine explizite PID und darf nicht mit
  `--process-name` kombiniert werden.
- `--help` oder `-h` zeigt die Kommandozeilenhilfe an.

Es kann immer nur ein Client mit dem Broker verbunden sein. Pro Client sind maximal
64 eindeutige PIDs desselben Windows-Benutzers aktiv. Mit `Strg+C` wird auch die
anfängliche Wartephase oder die laufende Aufzeichnung beendet. Nach dem ersten
Treffer stoppt der Client ebenfalls, wenn alle erkannten Zielprozesse beendet
wurden und er Synchronisationshandles auf sie öffnen konnte. Beendete Prozesse
werden aus der aktiven PID-Liste entfernt.

## CSV-Felder

Die CSV enthält genau diese Spalten:

- UTC-Zeitpunkt mit 100-ns-Auflösung aus der Windows-Systemzeit
- vom Client ermittelter Prozesspfad
- `Read` oder `Write`
- Erfolgsfeld (`1` oder `0`)
- normalisierten Dateipfad. Bekannte NT-Gerätepräfixe werden im Client über eine
  beim Start gecachte Zuordnung in Laufwerkspfade wie `C:\Users\...` umgewandelt;
  unbekannte NT-Pfade bleiben unverändert

## Beenden und entfernen

Zuerst den Client beenden und danach diesen Aufruf ausführen:

```powershell
sudo pwsh.exe -File .\install-service.ps1 -Uninstall
```

Das Skript erledigt den vollständigen Ablauf automatisch: Broker stoppen und
löschen, Minifilter entladen, das IoMonitor-OEM-Paket über mehrere
projektspezifische INF-Merkmale identifizieren und mit
`pnputil /delete-driver ... /uninstall` entfernen. Andere Treiber derselben Klasse
`ActivityMonitor` werden nicht anhand der Klasse allein ausgewählt.

Das WDK-Testzertifikat wird bewusst nicht automatisch entfernt, weil dasselbe
Zertifikat auch andere lokal gebaute Testtreiber signieren kann.

## Bewusste Grenzen des MVP

- Der Client wartet bei Bedarf auf den ersten Prozess mit dem angegebenen EXE-Namen
  und übernimmt dann alle zu diesem Zeitpunkt gefundenen PIDs. Noch später gestartete
  gleichnamige Prozesse werden nicht automatisch ergänzt; Unterprozesse mit anderem
  Namen ebenfalls nicht.
- Bei PID-Wiederverwendung könnte ohne gültigen Prozesshandle kurz ein falscher
  Prozess erfasst werden. Der Broker prüft Benutzer und PID erneut, kann aber eine
  Wiederverwendung nach dieser Prüfung ebenfalls nicht vollständig ausschließen.
- Paging-I/O besitzt nicht immer einen anfordernden Thread. In diesem Fall liefert
  Windows PID 0; solche Ereignisse lassen sich nicht zuverlässig der Ziel-PID
  zuordnen und werden nicht protokolliert.
- Memory-Mapped-I/O und Lazy-Writer-Aktivität entsprechen daher nicht zwingend einem
  direkten logischen Zugriff des Zielprozesses.
- Die Namensauflösung darf in bestimmten I/O-Kontexten scheitern. Solche Ereignisse
  werden bereits im Minifilter verworfen und gelangen nicht in die CSV.
- Pfade sind im Protokoll auf 511 UTF-16-Zeichen begrenzt und werden gegebenenfalls
  markiert abgeschnitten.
- Der Pfadcache wird bei einer Umbenennung über dasselbe Stream-Handle verworfen.
  Erfolgt die Umbenennung gleichzeitig über ein anderes Handle, kann ein bereits
  gecachter Pfad bis zum Schließen des ursprünglichen Handles veraltet sein.
- Ein Absturz oder eine langsame CSV-Platte verliert höchstens Ereignisse; der
  Zielprozess wird bewusst nicht zurückgestaut.
- Der INF-Altitude `385201` ist ausschließlich ein Testwert. Microsoft muss vor
  einer Verteilung einen Altitude-Wert passend zur Load-Order-Gruppe zuweisen.

## Nächste Schritte für eine robuste Version

- Prozessidentität zusätzlich über Erstellungszeit oder Kernel-Prozessobjekt absichern
- Konfigurierbare Queue- und Batchgrößen sowie Laufzeitmetriken ergänzen
- Mehrere gleichzeitige Clients, Logrotation und belastbare Shutdown-Semantik ergänzen
- Driver Verifier, Static Driver Verifier, CodeQL und Lasttests in einer VM ausführen
- Produktionszertifikat, zugewiesenen Altitude und HLK-/Kompatibilitätstests einplanen
