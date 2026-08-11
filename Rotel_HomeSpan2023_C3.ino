/*********************************************************************************
 *  HOMESPAN

 *  MIT License
 *  
 *  Copyright (c) 2021-2022 Gregg E. Berman
 *  
 *  https://github.com/HomeSpan/HomeSpan
 *  
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *  
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *  
 ********************************************************************************/



#include "HomeSpan.h"
#include "RotelCommand.h"


rotelCallback _rotel;

// Explizite Prototypen: Arduinos Auto-Generierung scheitert an Funktionen
// mit eigenem Parametertyp (RotelUpdateEvent) und zerschiesst dann die Klassen
void WifiDone(int count);
void HomeKitUpdate(RotelUpdateEvent event);


// Pairing Code: Standard 466-37-726, aenderbar ueber das Web-Dashboard (Port 80)

const char * wymrfirmware = "1.2.4";

// Aktives Verstärkermodell; wird in setup() aus dem NVS geladen (Auswahl
// über das Web-Dashboard, Default A12/A14). Quellen/Namen kommen aus der
// Modelltabelle in RotelModels.h – Index+1 = HomeKit-Identifier.
const RotelModelDef *rotelModel = &ROTEL_MODELS[ROTEL_DEFAULT_MODEL];

// WLAN-Portal, Dashboard und GitHub-OTA (braucht _rotel, wymrfirmware, rotelModel)
#include "WebPortal.h"

struct InfoService : Service::AccessoryInformation {

  InfoService() : Service::AccessoryInformation(){
    new Characteristic::Identify();
    // Ohne Name-Characteristic schlaegt die Home-App beim Hinzufuegen den
    // generischen Kategorienamen "TV" vor; mit ihr erscheint "Rotel"
    new Characteristic::Name("Rotel");
    new Characteristic::Manufacturer("WYMR-Design");
    new Characteristic::Model(rotelModel->name);
    new Characteristic::FirmwareRevision(wymrfirmware);
  }
};

struct Speaker : Service::TelevisionSpeaker {
  SpanCharacteristic *volume;
  SpanCharacteristic *mute;

  // Hinweis: Mute/Active stehen nicht in HomeSpans REQ/OPT-Liste und erzeugen
  // beim Boot je eine Log-Warnung – funktionieren aber (siehe HomeSpan #869).
  // Seit iOS 18 bedient der Mute-Button des Remote-Widgets diese Characteristic.
  Speaker() : Service::TelevisionSpeaker(){
    new Characteristic::Active(1);
    new Characteristic::VolumeControlType(3);
    volume = new Characteristic::VolumeSelector();
    mute   = new Characteristic::Mute(false);
  }

  boolean update() override {
    if (volume->updated()) {
      // HAP VolumeSelector: 0 = lauter, 1 = leiser
      LOG1("Volume-Step: %s\n", volume->getNewVal() ? "down" : "up");
      _rotel.setVolumeStep(volume->getNewVal() ? 0 : 1);
    }
    if (mute->updated()) {
      LOG1("Mute: %d\n", mute->getNewVal());
      _rotel.setMute(mute->getNewVal());
    }
    return (true);
  }
};

// Zeiger auf die Speaker-Instanz für den Mute-Rück-Sync in HomeKitUpdate()
Speaker *rotelSpeaker = nullptr;


// Eingangsquelle mit persistentem Namen und ein-/ausblendbarer Sichtbarkeit.
// nvsStore=true legt die Werte im NVS ab (Schlüssel = feste aid/iid, daher
// Reihenfolge der Service-Erzeugung in setup() nicht ändern!). Einmalige
// Allokation beim Boot, kein Heap-Verkehr zur Laufzeit.
struct RotelInput : Service::InputSource {

  SpanCharacteristic *targetVis;
  SpanCharacteristic *currentVis;

  RotelInput(uint8_t id, const char *name) : Service::InputSource() {
    new Characteristic::ConfiguredName(name, true);       // Umbenennung in der Home-App überlebt Reboot
    new Characteristic::Identifier(id);
    new Characteristic::IsConfigured(1);
    targetVis  = new Characteristic::TargetVisibilityState(0, true);   // Checkbox im TV-Einstellungsmenü, NVS-persistent
    currentVis = new Characteristic::CurrentVisibilityState(targetVis->getVal());  // Startwert = gespeicherte Checkbox
  }

  boolean update() override {
    if (targetVis->updated()) {
      currentVis->setVal(targetVis->getNewVal());   // Checkbox -> Sichtbarkeit in der Quellenliste
    }
    return true;
  }
};


struct HomeSpanTV : Service::Television {

  SpanCharacteristic *active = new Characteristic::Active();                  // Verstärker An/Aus (Start: Aus)
  SpanCharacteristic *activeID = new Characteristic::ActiveIdentifier(1);     // Startwert 1 ("CD"), bis der echte Wert vom Rotel kommt
  SpanCharacteristic *remoteKey = new Characteristic::RemoteKey();            // Tasten des Remote-Widgets

  // Auswahl für die Pfeiltasten L/R; folgt dem Bypass-Zustand (siehe HomeKitUpdate):
  // Bypass aktiv (Klangregelung aus) -> nur Balance; Bypass aus -> Start mit Bass
  // (kein "enum : uint8_t" – die Basistyp-Syntax verwirrt Arduinos Präprozessor)
  enum ToneSelect { TONE_BASS = 0, TONE_TREBLE, TONE_BALANCE };
  uint8_t toneSelect = TONE_BALANCE;

  // Zeigt den selektierten Parameter im Verstärker-Display an (Blink-Trick)
  void showToneSelection(){
    switch (toneSelect) {
      case TONE_BASS:   _rotel.blinkBass();    break;
      case TONE_TREBLE: _rotel.blinkTreble();  break;
      default:          _rotel.blinkBalance(); break;
    }
  }

  HomeSpanTV(const char *name) : Service::Television() {
    new Characteristic::ConfiguredName(name, true);   // TV-Name persistent (NVS)

    // DisplayOrder: feste Reihenfolge der Quellen in der Home-App
    // (TLV8: Tag 1 = Identifier, Tag 0 = Trenner). Das TLV-Objekt ist lokal,
    // HomeSpan kopiert den Wert einmalig – keine Laufzeit-Allokation.
    // Endstufen haben keine Quellen -> Characteristic ganz weglassen.
    if (rotelModel->sourceCount > 0) {
      TLV8 orderTLV;
      for (uint8_t i = 0; i < rotelModel->sourceCount; i++) {
        if (i > 0) orderTLV.add(0);
        orderTLV.add(1, i + 1);
      }
      new Characteristic::DisplayOrder(orderTLV);
    }
  }

  boolean update() override {



    if (active->updated()) {
      LOG1("TV Power: %d\n", active->getNewVal());
      _rotel.setPower(active->getNewVal());
    }

    if (activeID->updated()) {
      LOG1("Input Source: %d\n", activeID->getNewVal());
      _rotel.setSource(activeID->getNewVal());
    }

    if (remoteKey->updated()) {
      LOG1("Remote-Taste: %d (tone=%d)\n", remoteKey->getNewVal(), toneSelect);
      // Endstufen (M8/S5) haben weder Volume noch Klangregelung -> Tasten ignorieren
      const bool hasVolume = !_rotel.isPowerAmp();
      const bool hasTone   = rotelModel->hasTone;
      switch (remoteKey->getNewVal()) {
        case 4:   // UP: lauter
          if (hasVolume) _rotel.setVolumeStep(1);
          break;
        case 5:   // DOWN: leiser
          if (hasVolume) _rotel.setVolumeStep(0);
          break;
        case 6:   // LEFT: selektierten Klangparameter verringern
          if (!hasTone) break;
          switch (toneSelect) {
            case TONE_BASS:   _rotel.setBassStep(false);   break;
            case TONE_TREBLE: _rotel.setTrebleStep(false); break;
            default:          _rotel.setBalance(0);        break;
          }
          break;
        case 7:   // RIGHT: selektierten Klangparameter erhöhen
          if (!hasTone) break;
          switch (toneSelect) {
            case TONE_BASS:   _rotel.setBassStep(true);   break;
            case TONE_TREBLE: _rotel.setTrebleStep(true); break;
            default:          _rotel.setBalance(1);       break;
          }
          break;
        case 8:   // SELECT: Bypass an -> Klangmodus aktivieren (bypass_off);
                  //         Bypass aus -> Bass/Treble/Balance durchschalten
          if (!hasTone) break;
          if (_rotel._currentBypass) {
            _rotel.toggleBypass();   // Klangregelung aktivieren; Auswahl springt per Event auf Bass
          } else {
            toneSelect = (uint8_t)((toneSelect + 1) % 3);
            showToneSelection();
          }
          break;
        case 9:   // BACK: Klangmodus verlassen -> Bypass wieder aktivieren
          if (!hasTone) break;
          if (!_rotel._currentBypass) {
            _rotel.toggleBypass();   // Klangregelung deaktivieren; Auswahl springt per Event auf Balance
          }
          break;
        case 11:  // PLAY/PAUSE: frei
        case 15:  // INFO: frei
        default:
          break;
      }
    }

    return (true);
  }

  // Kein loop() mehr: Zustandssync passiert eventgetrieben in HomeKitUpdate()
};

// Zeiger auf die TV-Instanz, damit HomeKitUpdate() setVal() aufrufen kann
HomeSpanTV *rotelTV = nullptr;

///////////////////////////////

void setup() {

  Serial.begin(115200);

  homeSpan.setControlPin(9);
  // Kein deleteStoredValues() mehr: würde die NVS-gespeicherten Werte
  // (Quellen-Namen, Sichtbarkeit) bei jedem Boot löschen

  // Modellauswahl aus dem NVS (Dashboard-Karte "Verstärker"); bestimmt
  // Quellenliste, Protokollgeneration und Gerätetyp (Vollverstärker/Endstufe)
  rotelModel = &ROTEL_MODELS[rotelLoadModelIndex()];
  LOG0("Rotel-Modell: %s\n", rotelModel->name);

  _rotel.rotelCallbackUpdate(HomeKitUpdate);
  _rotel.setModel(rotelModel);   // setzt Dialekt bzw. startet Auto-Erkennung
  _rotel.rotelInitSwSer();


  // WLAN besitzt WiFiManagerLite (siehe WebPortal.h) – daher kein
  // enableAutoStartAP() mehr; HomeSpan erkennt die Verbindung ueber Events.
  // HAP zieht auf Port 8080 um: Port 80 gehoert dem Portal/Dashboard
  // (Captive-Portal-Erkennung funktioniert nur auf Port 80). HomeKit ist
  // der Port egal, er wird per mDNS bekanntgegeben.
  homeSpan.setPortNum(8080);

  // mDNS-Hostname statt "HomeSpan-<MAC>": Standard "Rotel-Remote" ->
  // http://rotel-remote.local; ueber die Dashboard-Karte "Geraete-Adresse"
  // aenderbar (NVS, wpLoadHostName). Leerer Suffix = keine MAC.
  wpLoadHostName();
  homeSpan.setHostNameSuffix("");
  homeSpan.begin(Category::Television, "Rotel Remote", wpHostName);
  
  
  homeSpan.setConnectionCallback(WifiDone);
  

  new SpanAccessory(); 
    new InfoService();

  // InputSources aus der Modelltabelle erzeugen; Identifier (Index+1) bleibt
  // automatisch deckungsgleich mit sources[] in RotelModels.h.
  // Endstufen (POWER_AMP) haben weder Quellen noch Lautsprecher-Service:
  // der TV-Service liefert dann nur den Ein/Aus-Schalter.
  SpanService *inputs[ROTEL_MAX_SOURCES];
  for (uint8_t i = 0; i < rotelModel->sourceCount; i++) {
    inputs[i] = new RotelInput(i + 1, rotelModel->sources[i].name);
  }

  if (rotelModel->deviceType == ROTEL_FULL_AMP) {
    rotelSpeaker = new Speaker();
  }

  // Neutraler HomeKit-Name (Modellnamen enthalten '/' und '(' – von der
  // Home-App nicht erlaubt); Umbenennung durch den Nutzer bleibt NVS-persistent
  rotelTV = new HomeSpanTV("Rotel");   // Television Service, InputSources müssen verlinkt sein
  for (uint8_t i = 0; i < rotelModel->sourceCount; i++) {
    rotelTV->addLink(inputs[i]);
  }
  if (rotelSpeaker) {
    rotelTV->addLink(rotelSpeaker);
  }

  // WLAN-Manager, Captive Portal und Dashboard starten (nach homeSpan.begin(),
  // damit HomeSpans Netzwerk-Event-Queue die Verbindungs-Events mitbekommt)
  webPortalSetup();

}


// Wird bei jeder (Re-)Verbindung aufgerufen (count = Anzahl der Verbindungen).
// Aktiviert rs232_update_on! (Push-Modus) und fragt den Grundzustand ab;
// die TX-Queue sendet antwortgesteuert (min. 100 ms Abstand). Auch nach einem
// WLAN-Ausfall sinnvoll: synchronisiert den HomeKit-Zustand neu.
void WifiDone(int count){
  _rotel.requestInitialState();
}


// Eventgetriebener Zustandssync: setVal() nur bei tatsächlicher Änderung
// (vermeidet unnötige HomeKit-Notifications). Läuft im loop()-Task, daher
// unkritisch gegenüber homeSpan.poll().
void HomeKitUpdate(RotelUpdateEvent event) {

  if (rotelTV == nullptr) return;   // Events vor Ende von setup() ignorieren

  switch (event) {
    case HOMEKIT_UPDATE_POWER:
      if (rotelTV->active->getVal() != (int)_rotel._currentPower) {
        rotelTV->active->setVal(_rotel._currentPower);
      }
      break;
    case HOMEKIT_UPDATE_SOURCE:
      if (rotelTV->activeID->getVal() != _rotel._currentSource) {
        rotelTV->activeID->setVal(_rotel._currentSource);
      }
      break;
    case HOMEKIT_UPDATE_MUTE:
      if (rotelSpeaker && rotelSpeaker->mute->getVal() != (int)_rotel._currentMute) {
        rotelSpeaker->mute->setVal(_rotel._currentMute);
      }
      break;
    case HOMEKIT_UPDATE_BYPASS:
      // Auswahl folgt dem Gerätezustand (greift auch bei Änderung am Gerät):
      // Bypass an -> nur Balance; Bypass aus -> automatisch Bass
      rotelTV->toneSelect = _rotel._currentBypass ? HomeSpanTV::TONE_BALANCE
                                                  : HomeSpanTV::TONE_BASS;
      break;
    default:
      break;
  }

}


///////////////////////////////

void loop() {
  _rotel.rotelLoop();   // RS232 empfangen + TX-Queue abarbeiten

  homeSpan.poll();

  webPortalLoop();      // WLAN-Manager, Portal, Update-/Reset-Auftraege
}
