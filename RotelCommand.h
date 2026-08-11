/*
 * rotelCommand.h
 *
 *  Created on: 2021-01-15
 *      Author: Martin Lihs
 *
 *  Rotel RS232, Gen 1 + Gen 2 (Modellauswahl siehe RotelModels.h):
 *    Gen 2 (A12/A14, RA-6000, Michi ...): Befehle "cmd!", Antworten "key=value$"
 *    Gen 1 (RA-12, RA-1570, alte 1572/1592-FW): Antworten "key=value!" bzw.
 *           Byte-Count-Format "key={len},{text}" ohne Terminator
 *
 *  RX-Parser und TX-Queue arbeiten ausschliesslich mit festen char-Puffern
 *  (keine Heap-Allokation im Hotpath -> keine Fragmentierung im 24/7-Betrieb).
 */

#ifndef ROTEL_Command_h
#define ROTEL_Command_h

#include "Arduino.h"
#include <HardwareSerial.h> // ESP32: HW-UART, SoftwareSerial schafft keine 115200
#include "RotelModels.h"

// LEOTRO ESP32-C3 RS232 Adapter Rev 1.1: UART -> SP3232 -> D-Sub
// begin(baud, cfg, rxPin, txPin) – Reihenfolge ist zwingend (rx, tx)
constexpr uint8_t RS232_RX_PIN = 4;   // <- RS232 (SP3232 R1OUT)
constexpr uint8_t RS232_TX_PIN = 10;  // -> RS232 (SP3232 T1IN)
// GPIO9 = BOOT-Taster -> homeSpan.setControlPin(9), nicht UART

constexpr uint32_t BAUD_RATE = 115200;

// Parser-Limits: laengster Key "product_version" (15); Byte-Count-Werte
// (Gen-1-Display) werden bei Ueberlaenge abgeschnitten, nicht verworfen
constexpr uint8_t CMD_BUF_LEN = 20;
constexpr uint8_t VAL_BUF_LEN = 32;

// TX-Queue: Rotel hat kein Flow Control -> Befehle sequenziell senden und auf
// die Antwort warten (Doku: sonst Paketverlust; bei Befehlsflut resettet die
// Geraete-CPU sogar). Naechster Befehl erst nach Antwort ODER Timeout.
constexpr uint8_t  TX_QUEUE_LEN        = 16;   // Init-Burst (9) + Re-Query nach Power-on (7, teils schon gesendet) passt
constexpr uint8_t  TX_CMD_MAX          = 24;   // laengster Befehl "display_update_auto!" = 20 Zeichen
constexpr uint32_t MIN_CMD_INTERVAL_MS = 100;  // Mindestabstand laut Doku (50-100 ms)
constexpr uint32_t RESPONSE_TIMEOUT_MS = 300;  // Fallback, falls keine Antwort kommt (z. B. Tastenbefehle)

// Auto-Erkennung Gen 1/Gen 2 (nur bei Modellen mit ROTEL_GEN_AUTO)
constexpr uint32_t DETECT_TIMEOUT_MS = 1000;   // Wartezeit pro Probe-Befehl
constexpr uint32_t DETECT_BACKOFF_MS = 15000;  // Pause, wenn beide Dialekte stumm bleiben

// Wertebereiche laut Protokoll; Balance-Maximum ist modellabhaengig
constexpr int TONE_MAX = 10;   // bass/treble: -10..000..+10 (alle Modelle)

// Frame-Zeichen
#define SENDEND        '!'
#define RECEIVEEND     '$'   // Gen 2; Gen 1 nutzt '!' (siehe frameTerminator())
#define RECEIVEDEVIDER '='

// ---- Befehle, die in beiden Generationen identisch sind ----
#define ROTEL_POWER_ON     "power_on!"
#define ROTEL_POWER_OFF    "power_off!"
#define ROTEL_POWER_TOGGLE "power_toggle!"
#define ROTEL_MUTE_ON      "mute_on!"
#define ROTEL_MUTE_OFF     "mute_off!"
#define ROTEL_MUTE_TOGGLE  "mute!"
#define ROTEL_BASS_UP      "bass_up!"
#define ROTEL_BASS_DOWN    "bass_down!"
#define ROTEL_BASS_ZERO    "bass_000!"
#define ROTEL_TREBLE_UP    "treble_up!"
#define ROTEL_TREBLE_DOWN  "treble_down!"
#define ROTEL_TREBLE_ZERO  "treble_000!"
#define ROTEL_BALANCE_ZERO "balance_000!"
#define ROTEL_DIMMER_TOGGLE "dimmer!"

// ---- RX-Schluessel (setStatus wertet beide Dialekte parallel aus) ----
#define ROTEL_POWER_RECEIVE   "power"
#define ROTEL_VOLUME_RECEIVE  "volume"
#define ROTEL_MUTE_RECEIVE    "mute"
#define ROTEL_SOURCE_RECEIVE  "source"
#define ROTEL_BALANCE_RECEIVE "balance"
#define ROTEL_BYPASS_RECEIVE  "bypass"           // Gen 2
#define ROTEL_TONE_RECEIVE    "tone"             // Gen 1 (invertierte Logik!)
#define ROTEL_BASS_RECEIVE    "bass"
#define ROTEL_TREBLE_RECEIVE  "treble"
#define ROTEL_FIRMWARE_SYSTEM_RECEIVE "version"          // Gen 2
#define ROTEL_FIRMWARE_GEN1_RECEIVE   "product_version"  // Gen 1 (Byte-Count)
#define ROTEL_FIRMWARE_USB_RECEIVE    "pc_version"       // nur Gen 2

// Befehls-Paar Gen 2 / Gen 1: Auswahl zur Laufzeit ueber pick()
struct RotelCmdPair {
  const char *gen2;
  const char *gen1;
};

static const RotelCmdPair CMDP_POWER_QUERY   = { "power?",           "get_current_power!"   };
static const RotelCmdPair CMDP_VOLUME_UP     = { "vol_up!",          "volume_up!"           };
static const RotelCmdPair CMDP_VOLUME_DOWN   = { "vol_dwn!",         "volume_down!"         };
static const RotelCmdPair CMDP_VOLUME_QUERY  = { "volume?",          "get_volume!"          };
static const RotelCmdPair CMDP_MUTE_QUERY    = { "mute?",            "get_mute_status!"     };
static const RotelCmdPair CMDP_SOURCE_QUERY  = { "source?",          "get_current_source!"  };
static const RotelCmdPair CMDP_TONE_ENABLE   = { "bypass_off!",      "tone_on!"  };  // Klangregelung EIN (invertierte Logik!)
static const RotelCmdPair CMDP_TONE_DISABLE  = { "bypass_on!",       "tone_off!" };  // Klangregelung AUS
static const RotelCmdPair CMDP_TONE_QUERY    = { "bypass?",          "get_tone!"            };
static const RotelCmdPair CMDP_BASS_QUERY    = { "bass?",            "get_bass!"            };
static const RotelCmdPair CMDP_TREBLE_QUERY  = { "treble?",          "get_treble!"          };
static const RotelCmdPair CMDP_BALANCE_L     = { "balance_l!",       "balance_left!"        };
static const RotelCmdPair CMDP_BALANCE_R     = { "balance_r!",       "balance_right!"       };
static const RotelCmdPair CMDP_BALANCE_QUERY = { "balance?",         "get_balance!"         };
static const RotelCmdPair CMDP_UPDATE_AUTO   = { "rs232_update_on!", "display_update_auto!" };
static const RotelCmdPair CMDP_VERSION_QUERY = { "version?",         "get_product_version!" };

#define ROTEL_FIRMWARE_USB_STATE "pc_version?"   // nur Gen 2


enum RotelUpdateEvent {
  HOMEKIT_UPDATE_NONE = 0,
  HOMEKIT_UPDATE_ALL,
  HOMEKIT_UPDATE_VOLUME,
  HOMEKIT_UPDATE_MUTE,
  HOMEKIT_UPDATE_POWER,
  HOMEKIT_UPDATE_SOURCE,
  HOMEKIT_UPDATE_DIMMER,
  HOMEKIT_UPDATE_BYPASS,
  HOMEKIT_UPDATE_BALANCE,
  HOMEKIT_UPDATE_BASS,
  HOMEKIT_UPDATE_TREBLE,
  HOMEKIT_UPDATE_FIRMWARE_USB,
  HOMEKIT_UPDATE_FIRMWARE_SYSTEM,
};

typedef void (*callbackUpdateFunction)(RotelUpdateEvent);


class rotelCallback {
public:

    bool _currentPower = false;
    int _currentVolume = 0;
    int _currentBalance = 0;
    int _currentBass = 0;
    int _currentTreble = 0;
    bool _currentMute = false;
    int  _currentSource = 0;
    bool _currentBypass = false;   // true = Klangregelung AUS (Gen-2-Sicht)
    char _currentFirmwareSystem[VAL_BUF_LEN] = "n.a.";
    char _currentFirmwareUSB[VAL_BUF_LEN] = "n.a.";

    void rotelCallbackUpdate(callbackUpdateFunction newFunction) {
      _updateCallback = newFunction;
    }

    // Muss VOR rotelInitSwSer()/loop() gesetzt werden (Quellenliste, Dialekt).
    // Bei ROTEL_GEN_AUTO: NVS-Cache nutzen, sonst Erkennung starten (non-blocking).
    void setModel(const RotelModelDef *m){
      _model = m;
      if (m->generation == ROTEL_GEN1) {
        _gen1Active = true;
        _detectState = DETECT_OFF;
      } else if (m->generation == ROTEL_GEN2) {
        _gen1Active = false;
        _detectState = DETECT_OFF;
      } else {  // ROTEL_GEN_AUTO
        const uint8_t cached = rotelLoadGenCache();
        if (cached != ROTEL_GEN_UNKNOWN) {
          _gen1Active = (cached == ROTEL_GEN1);
          _detectState = DETECT_DONE;
          LOG1("ROTEL: Generation aus NVS-Cache: Gen %d\n", _gen1Active ? 1 : 2);
        } else {
          // Provisorisch Gen 2 (haeufigster Fall bei aktueller Firmware);
          // die Probe laeuft parallel im loop() und korrigiert bei Bedarf.
          _gen1Active = false;
          startProbe(DETECT_PROBE_G2);
        }
      }
    }

    const RotelModelDef* model() const { return _model; }
    bool isGen1() const { return _gen1Active; }
    bool isPowerAmp() const { return _model->deviceType == ROTEL_POWER_AMP; }

    // Fuer Dashboard-Anzeige (ASCII, statischer Text)
    const char* generationLabel() const {
      if (_detectState == DETECT_PROBE_G2 || _detectState == DETECT_PROBE_G1 ||
          _detectState == DETECT_BACKOFF) {
        return "detecting...";
      }
      return _gen1Active ? "Gen 1 (legacy)" : "Gen 2";
    }

    void rotelInitSwSer(){
      // Groesserer RX-Puffer: HomeSpan-Krypto (Pairing) kann loop() >100 ms
      // blockieren; bei 115200 Baud waeren 256 Bytes sonst schnell voll.
      // Muss VOR begin() gesetzt werden.
      _serial.setRxBufferSize(1024);
      // Hardware-UART1 auf fest verdrahtete RS232-Pins (siehe AGENTS.md)
      _serial.begin(BAUD_RATE, SERIAL_8N1, RS232_RX_PIN, RS232_TX_PIN);
      LOG1("RS232 UART1 gestartet (RX=4, TX=10)\n");
    }

    // Muss in jedem loop()-Durchlauf laufen: Antworten parsen + Queue senden
    void rotelLoop(){
      rotelRecive();
      processDetect();
      processTx();
    }

    // Nach WiFi/Boot: Push-Modus aktivieren + Grundzustand abfragen.
    // Laeuft die Generation-Erkennung noch, wird der Burst zurueckgestellt
    // und nach erfolgreicher Erkennung automatisch nachgeholt.
    void requestInitialState(){
      if (_detectState == DETECT_PROBE_G2 || _detectState == DETECT_PROBE_G1 ||
          _detectState == DETECT_BACKOFF) {
        _initPending = true;
        return;
      }
      queueCommand(pick(CMDP_UPDATE_AUTO));
      queueCommand(pick(CMDP_POWER_QUERY));
      requestStateQueries();
    }

    // Zustandsabfragen (ohne Power); wird auch nach dem Einschalten aus
    // Standby erneut ausgefuehrt, weil viele Modelle im Standby nicht antworten.
    // Endstufen (POWER_AMP) haben weder Quellen noch Volume/Klangregelung.
    void requestStateQueries(){
      if (isPowerAmp()) return;
      queueCommand(pick(CMDP_VOLUME_QUERY));   // sonst bleibt _currentVolume nach Boot auf 0
      queueCommand(pick(CMDP_SOURCE_QUERY));
      queueCommand(pick(CMDP_MUTE_QUERY));
      if (_model->hasTone) {
        queueCommand(pick(CMDP_TONE_QUERY));
        queueCommand(pick(CMDP_BASS_QUERY));
        queueCommand(pick(CMDP_TREBLE_QUERY));
      }
      queueCommand(pick(CMDP_BALANCE_QUERY));
    }

    // ---- RX: Zustandsmaschine mit festen Puffern (kein String, kein Heap) ----
    // Gen 2: "key=value$"  |  Gen 1: "key=value!" oder "key={len},{bytes}"
    void rotelRecive(){
      while (_serial.available() > 0) {
        const char c = (char)_serial.read();

        // Gen-1-Byte-Count-Modus: exakt N Rohbytes lesen (kein Terminator)
        if (_byteCountRemaining > 0) {
          if (_valLen < VAL_BUF_LEN - 1) _valBuf[_valLen++] = c;  // Ueberlaenge abschneiden
          _byteCountRemaining--;
          if (_byteCountRemaining == 0) finishFrame();
          continue;
        }

        if (c == frameTerminator()) {             // '$' (Gen 2) bzw. '!' (Gen 1)
          finishFrame();
        } else if (c == RECEIVEDEVIDER) {         // '=' trennt Key/Value
          _readingValue = true;
          _valDigitsOnly = true;
        } else if (c >= ' ') {                    // Steuerzeichen (CR/LF) ignorieren
          if (!_readingValue) {
            if (c == ' ') continue;               // Leerzeichen im Key ueberspringen (ersetzt trim)
            if (_cmdLen < CMD_BUF_LEN - 1) {
              _cmdBuf[_cmdLen++] = c;
            } else {
              _discardFrame = true;               // Overflow/Noise: Frame bis Terminator verwerfen
            }
          } else if (_gen1Active && c == ',' && _valDigitsOnly && _valLen > 0 && _valLen <= 3) {
            // Gen 1: "key={len},{bytes}" -> Laengenpraefix erkannt
            _valBuf[_valLen] = '\0';
            _byteCountRemaining = (int16_t)atoi(_valBuf);
            _valLen = 0;
            if (_byteCountRemaining <= 0) finishFrame();   // "key=0," -> leerer Wert
          } else {
            if (c < '0' || c > '9') _valDigitsOnly = false;
            if (_valLen < VAL_BUF_LEN - 1) {
              _valBuf[_valLen++] = c;
            } else {
              _discardFrame = true;
            }
          }
        }
      }
    }


    /* ------ Power Getter Setter -----*/
    void setPower(bool pwr){
      queueCommand(pwr ? ROTEL_POWER_ON : ROTEL_POWER_OFF);
    }

    void getPower(){
      queueCommand(pick(CMDP_POWER_QUERY));
    }

    void updatePower(bool pwr){
      const bool wasOn = _currentPower;
      _currentPower = pwr;
      notifyEvent(HOMEKIT_UPDATE_POWER);
      // Nach Einschalten aus Standby den Gesamtzustand nachziehen:
      // im Standby beantworten viele Modelle keine Queries (Boot mit Amp aus)
      if (pwr && !wasOn) {
        requestStateQueries();
      }
    }

    void PowerToggle(){
      queueCommand(ROTEL_POWER_TOGGLE);
    }


    /* ------ Volume Getter Setter -----*/
    void setVolumeNN(int vol){
      // Zweistellig; gueltiger Bereich laut Doku 01-96. Fuer 0 gibt es den
      // dedizierten min-Befehl ("vol_00!" ist nicht spezifiziert).
      if (vol < 0)  vol = 0;
      if (vol > 96) vol = 96;
      char cmd[TX_CMD_MAX];
      if (vol == 0) strlcpy(cmd, _gen1Active ? "volume_min!" : "vol_min!", sizeof(cmd));
      else          snprintf(cmd, sizeof(cmd), _gen1Active ? "volume_%02d!" : "vol_%02d!", vol);
      queueCommand(cmd);
    }

    void setVolumeStep(bool up){
      queueCommand(pick(up ? CMDP_VOLUME_UP : CMDP_VOLUME_DOWN));
    }

    void getVolume(){
      queueCommand(pick(CMDP_VOLUME_QUERY));
    }

    void updateVolume(int vol){
      _currentVolume = vol;
      notifyEvent(HOMEKIT_UPDATE_VOLUME);
    }


    /* ------ BYPASS Getter Setter -----*/
    // Interne Semantik immer Gen 2: _currentBypass=true -> Klangregelung AUS.
    // Die invertierte Gen-1-Logik (tone_on = Klangregelung EIN) kapselt pick().
    void toggleBypass() {
      queueCommand(pick(_currentBypass ? CMDP_TONE_ENABLE : CMDP_TONE_DISABLE));
    }

    void updateBypass(bool bp){
      _currentBypass = bp;
      notifyEvent(HOMEKIT_UPDATE_BYPASS);
    }

    void getBypass(){
      queueCommand(pick(CMDP_TONE_QUERY));
    }


    /* ------ Mute Getter Setter -----*/
    void setMute(bool mute){
      queueCommand(mute ? ROTEL_MUTE_ON : ROTEL_MUTE_OFF);
    }

    void toggleMute(){
      queueCommand(ROTEL_MUTE_TOGGLE);
    }

    void getMute(){
      queueCommand(pick(CMDP_MUTE_QUERY));
    }

    void updateMute(bool mute){
      _currentMute = mute;
      notifyEvent(HOMEKIT_UPDATE_MUTE);
    }


    /* ------ Balance Getter Setter -----*/
    void setBalance(bool right){
      queueCommand(pick(right ? CMDP_BALANCE_R : CMDP_BALANCE_L));
    }

    void zeroBalance(){
      queueCommand(ROTEL_BALANCE_ZERO);
    }

    void getBalance(){
      queueCommand(pick(CMDP_BALANCE_QUERY));
    }

    void setBalanceValue(int balance){
      // Bereich modellabhaengig (A14: +-15, Michi X: +-10).
      // Gen 2 kleinbuchstabig ("balance_r05!"), Gen 1 gross ("balance_R05!").
      const int maxBal = _model->balanceMax;
      if (balance >  maxBal) balance =  maxBal;
      if (balance < -maxBal) balance = -maxBal;
      _currentBalance = balance;
      char cmd[TX_CMD_MAX];
      if (balance > 0) {
        snprintf(cmd, sizeof(cmd), _gen1Active ? "balance_R%02d!" : "balance_r%02d!", balance);
      } else if (balance < 0) {
        snprintf(cmd, sizeof(cmd), _gen1Active ? "balance_L%02d!" : "balance_l%02d!", -balance);
      } else {
        strlcpy(cmd, ROTEL_BALANCE_ZERO, sizeof(cmd));
      }
      queueCommand(cmd);
    }

    void updateBalance(int balance){
      _currentBalance = balance;
      notifyEvent(HOMEKIT_UPDATE_BALANCE);
    }


    /* ------ Bass / Treble Getter Setter -----*/
    void setBassStep(bool up){
      queueCommand(up ? ROTEL_BASS_UP : ROTEL_BASS_DOWN);
    }

    void zeroBass(){
      queueCommand(ROTEL_BASS_ZERO);
    }

    void setBassValue(int bass){
      // Absolutwert statt vieler Einzelschritte: schnelle Klickfolgen aus dem
      // Web-UI werden zu EINEM Befehl (Doku: Befehlsflut kann die Geraete-CPU
      // resetten -> Verstaerker faellt in Standby). Format: "bass_+05!"/"bass_000!"
      if (bass >  TONE_MAX) bass =  TONE_MAX;
      if (bass < -TONE_MAX) bass = -TONE_MAX;
      char cmd[TX_CMD_MAX];
      if (bass == 0) strlcpy(cmd, ROTEL_BASS_ZERO, sizeof(cmd));
      else           snprintf(cmd, sizeof(cmd), "bass_%+03d!", bass);
      queueCommand(cmd);
    }

    void getBass(){
      queueCommand(pick(CMDP_BASS_QUERY));
    }

    void updateBass(int bass){
      _currentBass = bass;
      notifyEvent(HOMEKIT_UPDATE_BASS);
    }

    void setTrebleStep(bool up){
      queueCommand(up ? ROTEL_TREBLE_UP : ROTEL_TREBLE_DOWN);
    }

    void zeroTreble(){
      queueCommand(ROTEL_TREBLE_ZERO);
    }

    void setTrebleValue(int treble){
      // Absolutwert, siehe setBassValue()
      if (treble >  TONE_MAX) treble =  TONE_MAX;
      if (treble < -TONE_MAX) treble = -TONE_MAX;
      char cmd[TX_CMD_MAX];
      if (treble == 0) strlcpy(cmd, ROTEL_TREBLE_ZERO, sizeof(cmd));
      else             snprintf(cmd, sizeof(cmd), "treble_%+03d!", treble);
      queueCommand(cmd);
    }

    void getTreble(){
      queueCommand(pick(CMDP_TREBLE_QUERY));
    }

    void updateTreble(int treble){
      _currentTreble = treble;
      notifyEvent(HOMEKIT_UPDATE_TREBLE);
    }


    /* ------ "Blink-Trick": Display-Anzeige ohne Netto-Aenderung -----*/
    // Parameter kurz +1/-1 verstellen, damit das Geraetedisplay ihn anzeigt.
    // Reihenfolge randfallsicher: an der Obergrenze zuerst runter, sonst zuerst
    // hoch (an der Grenze wirkt der erste Befehl sonst nicht -> Netto-Aenderung).
    // Nur bei leerer Queue: verhindert Befehlsfluten und halbe Paare bei
    // schnellen Tastendruecken (Blink ist rein kosmetisch, darf entfallen).
    void blinkBalance(){
      if (_txCount > 0) return;
      if (_currentBalance >= _model->balanceMax) {
        queueCommand(pick(CMDP_BALANCE_L));
        queueCommand(pick(CMDP_BALANCE_R));
      } else {
        queueCommand(pick(CMDP_BALANCE_R));
        queueCommand(pick(CMDP_BALANCE_L));
      }
    }

    void blinkBass(){
      if (_txCount > 0) return;
      if (_currentBass >= TONE_MAX) {
        queueCommand(ROTEL_BASS_DOWN);
        queueCommand(ROTEL_BASS_UP);
      } else {
        queueCommand(ROTEL_BASS_UP);
        queueCommand(ROTEL_BASS_DOWN);
      }
    }

    void blinkTreble(){
      if (_txCount > 0) return;
      if (_currentTreble >= TONE_MAX) {
        queueCommand(ROTEL_TREBLE_DOWN);
        queueCommand(ROTEL_TREBLE_UP);
      } else {
        queueCommand(ROTEL_TREBLE_UP);
        queueCommand(ROTEL_TREBLE_DOWN);
      }
    }


    /* ------ Source Getter Setter -----*/
    void setSource(int src){
      if (src < 1 || src > _model->sourceCount) return;
      queueCommand(_model->sources[src - 1].command);
    }

    void getSource(){
      queueCommand(pick(CMDP_SOURCE_QUERY));
    }

    void updateSource(int src){
      _currentSource = src;
      notifyEvent(HOMEKIT_UPDATE_SOURCE);
    }


    /* ------ Firmware -----*/
    void getFirmwareSystem(){
      queueCommand(pick(CMDP_VERSION_QUERY));
    }

    void updateFirmwareSystem(const char* firmware){
      strlcpy(_currentFirmwareSystem, firmware, sizeof(_currentFirmwareSystem));
      notifyEvent(HOMEKIT_UPDATE_FIRMWARE_SYSTEM);
    }

    void getFirmwareUSB(){
      if (_gen1Active) return;   // pc_version? existiert nur in Gen 2
      queueCommand(ROTEL_FIRMWARE_USB_STATE);
    }

    void updateFirmwareUSB(const char* firmware){
      strlcpy(_currentFirmwareUSB, firmware, sizeof(_currentFirmwareUSB));
      notifyEvent(HOMEKIT_UPDATE_FIRMWARE_USB);
    }


private:
    HardwareSerial _serial{1};                 // UART1, Pins siehe oben

    callbackUpdateFunction _updateCallback = nullptr;

    // Aktives Modell (Default A14, bis setModel() aufgerufen wird)
    const RotelModelDef *_model = &ROTEL_MODELS[ROTEL_DEFAULT_MODEL];
    bool _gen1Active = false;

    // Auto-Erkennung Gen 1/Gen 2 (non-blocking, laeuft im rotelLoop)
    enum DetectState : uint8_t {
      DETECT_OFF = 0,     // Generation fest vorgegeben
      DETECT_PROBE_G2,    // Gen-2-Probe gesendet, warte auf Antwort
      DETECT_PROBE_G1,    // Gen-1-Probe gesendet, warte auf Antwort
      DETECT_BACKOFF,     // beide stumm (Amp aus/Kabel fehlt) -> spaeter erneut
      DETECT_DONE
    };
    DetectState _detectState = DETECT_OFF;
    uint32_t _detectStartMs = 0;
    bool _frameSeen = false;      // vollstaendiger Frame seit letzter Probe
    bool _initPending = false;    // requestInitialState() nachholen

    // RX-Parser-State (frueher globale Strings im Header)
    char _cmdBuf[CMD_BUF_LEN];
    char _valBuf[VAL_BUF_LEN];
    uint8_t _cmdLen = 0;
    uint8_t _valLen = 0;
    bool _readingValue = false;
    bool _discardFrame = false;
    bool _valDigitsOnly = false;
    int16_t _byteCountRemaining = 0;   // >0: Gen-1-Byte-Count-Modus aktiv

    // TX-Ringpuffer (statisch, einmal allokiert)
    char _txQueue[TX_QUEUE_LEN][TX_CMD_MAX];
    uint8_t _txHead = 0;
    uint8_t _txTail = 0;
    uint8_t _txCount = 0;
    uint32_t _lastTxMs = 0;
    bool _awaitingResponse = false;   // true = warten auf Antwort-Frame zum letzten Befehl

    const char* pick(const RotelCmdPair &p) const {
      return _gen1Active ? p.gen1 : p.gen2;
    }

    char frameTerminator() const {
      return _gen1Active ? SENDEND : RECEIVEEND;   // Gen 1: '!', Gen 2: '$'
    }

    // Frame komplett: auswerten und Parser zuruecksetzen
    void finishFrame(){
      _awaitingResponse = false;              // Antwort da -> naechster Befehl darf raus
      _frameSeen = true;                      // Signal fuer die Generation-Erkennung
      if (!_discardFrame) {
        _cmdBuf[_cmdLen] = '\0';
        _valBuf[_valLen] = '\0';
        setStatus(_cmdBuf, _valBuf);
      }
      resetParser();
    }

    void resetParser(){
      _cmdLen = 0;
      _valLen = 0;
      _readingValue = false;
      _discardFrame = false;
      _valDigitsOnly = false;
      _byteCountRemaining = 0;
    }

    // Probe fuer die Generation-Erkennung senden (Power-Abfrage im aktuell
    // angenommenen Dialekt); Antwort-Terminator bestaetigt die Generation.
    void startProbe(DetectState st){
      _detectState = st;
      _frameSeen = false;
      resetParser();
      _detectStartMs = millis();
      // Vor der Gen-1-Probe den Befehlspuffer des Geraets leeren: die Gen-2-
      // Probe "power?" hat keinen '!'-Terminator und wuerde sonst bei einem
      // Gen-1-Geraet mit dem Folgebefehl zu einem ungueltigen Kommando
      // verschmelzen ("power?get_current_power!"). Ein einzelnes '!' schliesst
      // den Puffer-Rest ab; Gen-2-Geraete ignorieren den leeren Befehl.
      if (st == DETECT_PROBE_G1) queueCommand("!");
      queueCommand(pick(CMDP_POWER_QUERY));
      LOG1("ROTEL: Generation-Probe Gen %d\n", _gen1Active ? 1 : 2);
    }

    // Erkennungs-Statemachine: pro Dialekt eine Probe, dann Backoff.
    // Kein Blockieren, keine Rekursion; millis()-Differenzen rollover-sicher.
    void processDetect(){
      if (_detectState == DETECT_OFF || _detectState == DETECT_DONE) return;

      const uint32_t now = millis();

      if (_frameSeen) {
        // Frame im aktiven Dialekt vollstaendig geparst -> Generation steht
        // fest. Gilt auch im Backoff: dort laeuft der Parser im Gen-2-Modus,
        // ein spontaner Frame (Amp wurde eingeschaltet) beendet das Warten.
        _detectState = DETECT_DONE;
        const uint8_t gen = _gen1Active ? ROTEL_GEN1 : ROTEL_GEN2;
        rotelSaveGenCache(gen);
        LOG0("ROTEL: Generation erkannt: Gen %d (im NVS gespeichert)\n", _gen1Active ? 1 : 2);
        if (_initPending) {
          _initPending = false;
          requestInitialState();
        }
        return;
      }

      if (_detectState == DETECT_BACKOFF) {
        if (now - _detectStartMs >= DETECT_BACKOFF_MS) {
          _gen1Active = false;
          startProbe(DETECT_PROBE_G2);
        }
        return;
      }

      // Solange die Probe noch in der TX-Queue steht (z. B. weil der HomeSpan-
      // Start den ersten loop() verzoegert hat), Timeout-Timer nachziehen:
      // die Wartezeit soll erst ab dem tatsaechlichen Senden laufen
      if (_txCount > 0) {
        _detectStartMs = now;
        return;
      }

      if (now - _detectStartMs >= DETECT_TIMEOUT_MS) {
        if (_detectState == DETECT_PROBE_G2) {
          _gen1Active = true;                 // naechster Versuch im Gen-1-Dialekt
          startProbe(DETECT_PROBE_G1);
        } else {
          // Beide Dialekte stumm: Verstaerker aus oder Kabel fehlt.
          // Bis zur Erkennung provisorisch Gen 2 verwenden.
          _gen1Active = false;
          _detectState = DETECT_BACKOFF;
          _detectStartMs = now;
        }
      }
    }

    void notifyEvent(RotelUpdateEvent ev){
      if (_updateCallback) _updateCallback(ev);
    }

    bool queueCommand(const char* cmd){
      if (_txCount >= TX_QUEUE_LEN) {
        // LOG0 = immer sichtbar: verworfener Befehl ist ein echter Fehlerfall
        LOG0("ROTEL: TX-Queue voll, Befehl verworfen\n");
        return false;
      }
      strlcpy(_txQueue[_txHead], cmd, TX_CMD_MAX);
      _txHead = (uint8_t)((_txHead + 1) % TX_QUEUE_LEN);
      _txCount++;
      return true;
    }

    // Antwortgesteuertes Pacing: naechster Befehl erst, wenn die Antwort zum
    // vorigen da ist (oder RESPONSE_TIMEOUT_MS abgelaufen), und nie schneller
    // als MIN_CMD_INTERVAL_MS. millis()-Differenzen sind rollover-sicher.
    void processTx(){
      if (_txCount == 0) return;
      const uint32_t now = millis();
      if (now - _lastTxMs < MIN_CMD_INTERVAL_MS) return;
      if (_awaitingResponse && (now - _lastTxMs) < RESPONSE_TIMEOUT_MS) return;
      _serial.print(_txQueue[_txTail]);
      _txTail = (uint8_t)((_txTail + 1) % TX_QUEUE_LEN);
      _txCount--;
      _lastTxMs = now;
      _awaitingResponse = true;
    }

    // Frame-Auswertung ohne Heap: strcmp/atoi statt String-Methoden.
    // Wertet Gen-1- und Gen-2-Schluessel parallel aus (unbekannte ignorieren).
    void setStatus(const char* cmd, const char* val){

      if (strcmp(cmd, ROTEL_VOLUME_RECEIVE) == 0) {
        updateVolume(atoi(val));
      } else if (strcmp(cmd, ROTEL_POWER_RECEIVE) == 0) {
        if (strcmp(val, "on") == 0)           { updatePower(true); }
        else if (strcmp(val, "standby") == 0) { updatePower(false); }
      } else if (strcmp(cmd, ROTEL_MUTE_RECEIVE) == 0) {
        if (strcmp(val, "on") == 0)       { updateMute(true); }
        else if (strcmp(val, "off") == 0) { updateMute(false); }
      } else if (strcmp(cmd, ROTEL_SOURCE_RECEIVE) == 0) {
        for (uint8_t i = 0; i < _model->sourceCount; i++) {
          if (strcmp(val, _model->sources[i].replyToken) == 0) {
            updateSource(i + 1);
            break;
          }
        }
      } else if (strcmp(cmd, ROTEL_BYPASS_RECEIVE) == 0) {
        // Gen 2: bypass=on -> Klangregelung AUS
        if (strcmp(val, "on") == 0)       { updateBypass(true); }
        else if (strcmp(val, "off") == 0) { updateBypass(false); }
      } else if (strcmp(cmd, ROTEL_TONE_RECEIVE) == 0) {
        // Gen 1: tone=on -> Klangregelung EIN (invertiert zu bypass!)
        if (strcmp(val, "on") == 0)       { updateBypass(false); }
        else if (strcmp(val, "off") == 0) { updateBypass(true); }
      } else if (strcmp(cmd, ROTEL_BALANCE_RECEIVE) == 0) {
        // Antwortformat: "R05" / "L05" / "000" (Gross-/Kleinschreibung tolerieren)
        if (val[0] == 'R' || val[0] == 'r')      { updateBalance(atoi(val + 1)); }
        else if (val[0] == 'L' || val[0] == 'l') { updateBalance(-atoi(val + 1)); }
        else                                     { updateBalance(0); }
      } else if (strcmp(cmd, ROTEL_BASS_RECEIVE) == 0) {
        updateBass(atoi(val));      // atoi verdaut "+05"/"-05"/"000" direkt
      } else if (strcmp(cmd, ROTEL_TREBLE_RECEIVE) == 0) {
        updateTreble(atoi(val));
      } else if (strcmp(cmd, ROTEL_FIRMWARE_SYSTEM_RECEIVE) == 0 ||
                 strcmp(cmd, ROTEL_FIRMWARE_GEN1_RECEIVE) == 0) {
        updateFirmwareSystem(val);
      } else if (strcmp(cmd, ROTEL_FIRMWARE_USB_RECEIVE) == 0) {
        updateFirmwareUSB(val);
      }
      // Unbekannte Keys (z. B. "freq", "display", "update_mode") bewusst ignorieren
    }

};


#endif
