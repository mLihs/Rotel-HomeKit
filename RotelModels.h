/*
 * RotelModels.h – Modelltabelle fuer Multi-Verstaerker-Support
 *
 * Beschreibt pro Modellfamilie: Protokollgeneration (bzw. AUTO fuer
 * firmwareabhaengige Modelle), Geraetetyp, Quellenliste und Grenzwerte.
 * Quelle: rotel-rs232-serial.md (offizielle Rotel-Protokolldokumente).
 *
 * Alle Tabellen liegen als const im Flash – keine Laufzeit-Allokation.
 * Auswahl und Generation-Cache liegen im NVS (Namespace "rotelcfg").
 *
 * Hinweis Quellenlisten: Die Eingaenge sind pro Modell grosszuegig gelistet;
 * ungenutzte blendet der Nutzer ueber die Checkboxen der Home-App aus.
 */

#ifndef ROTEL_MODELS_H
#define ROTEL_MODELS_H

#include "Arduino.h"
#include <Preferences.h>

// Protokollgeneration (Werte = NVS-Cache-Kodierung, 0 = unbekannt)
enum RotelGeneration : uint8_t {
  ROTEL_GEN_UNKNOWN = 0,
  ROTEL_GEN1        = 1,   // Antworten enden auf '!', Abfragen "get_*!"
  ROTEL_GEN2        = 2,   // Antworten enden auf '$', Abfragen "*?"
  ROTEL_GEN_AUTO    = 3    // firmwareabhaengig -> zur Laufzeit erkennen
};

enum RotelDeviceType : uint8_t {
  ROTEL_FULL_AMP  = 0,   // Vollverstaerker/Vorstufe: Volume, Quellen, Klang
  ROTEL_POWER_AMP = 1    // Endstufe (M8/S5): nur Power (+ Dimmer)
};

// HomeKit-Source-ID (Index+1) <-> Rotel-Token; Antwortwert kann vom Befehl
// abweichen (Befehl "pcusb!", Antwort "source=pc_usb")
struct RotelSourceDef {
  const char *replyToken;   // Wert in der Antwort "source=<token>"
  const char *command;      // zu sendender Befehl inkl. '!'
  const char *name;         // Anzeigename in der Home-App
};

struct RotelModelDef {
  const char *name;               // Anzeigename im Dashboard
  uint8_t     generation;         // ROTEL_GEN1 / ROTEL_GEN2 / ROTEL_GEN_AUTO
  uint8_t     deviceType;         // ROTEL_FULL_AMP / ROTEL_POWER_AMP
  const RotelSourceDef *sources;  // nullptr bei POWER_AMP
  uint8_t     sourceCount;
  int8_t      balanceMax;         // A14: 15, Michi X: 10
  bool        hasTone;            // Bass/Treble/Bypass vorhanden
};

// ---------- Quellenlisten ----------

static const RotelSourceDef ROTEL_SRC_A14[] = {
  { "cd",        "cd!",        "CD"        },
  { "coax1",     "coax1!",     "Coax 1"    },
  { "coax2",     "coax2!",     "Coax 2"    },
  { "opt1",      "opt1!",      "Optical 1" },
  { "opt2",      "opt2!",      "Optical 2" },
  { "aux1",      "aux1!",      "AUX 1"     },
  { "aux2",      "aux2!",      "AUX 2"     },
  { "tuner",     "tuner!",     "Tuner"     },
  { "phono",     "phono!",     "Phono"     },
  { "usb",       "usb!",       "USB"       },
  { "bluetooth", "bluetooth!", "Bluetooth" },
  { "pc_usb",    "pcusb!",     "PC USB"    },
};

static const RotelSourceDef ROTEL_SRC_A11[] = {
  { "cd",        "cd!",        "CD"        },
  { "coax1",     "coax1!",     "Coax 1"    },
  { "coax2",     "coax2!",     "Coax 2"    },
  { "opt1",      "opt1!",      "Optical 1" },
  { "opt2",      "opt2!",      "Optical 2" },
  { "aux1",      "aux1!",      "AUX 1"     },
  { "aux2",      "aux2!",      "AUX 2"     },
  { "tuner",     "tuner!",     "Tuner"     },
  { "phono",     "phono!",     "Phono"     },
  { "bluetooth", "bluetooth!", "Bluetooth" },
};

// RA-1572/RA-1592/RA-6000: wie A14 plus dritter Digital-Satz und XLR-Eingang
static const RotelSourceDef ROTEL_SRC_RA157X[] = {
  { "cd",        "cd!",        "CD"        },
  { "coax1",     "coax1!",     "Coax 1"    },
  { "coax2",     "coax2!",     "Coax 2"    },
  { "coax3",     "coax3!",     "Coax 3"    },
  { "opt1",      "opt1!",      "Optical 1" },
  { "opt2",      "opt2!",      "Optical 2" },
  { "opt3",      "opt3!",      "Optical 3" },
  { "aux1",      "aux1!",      "AUX 1"     },
  { "aux2",      "aux2!",      "AUX 2"     },
  { "tuner",     "tuner!",     "Tuner"     },
  { "phono",     "phono!",     "Phono"     },
  { "usb",       "usb!",       "USB"       },
  { "bluetooth", "bluetooth!", "Bluetooth" },
  { "pc_usb",    "pcusb!",     "PC USB"    },
  { "bal_xlr",   "bal_xlr!",   "XLR"       },
};

static const RotelSourceDef ROTEL_SRC_MICHI_X[] = {
  { "coax1",     "coax1!",     "Coax 1"    },
  { "coax2",     "coax2!",     "Coax 2"    },
  { "opt1",      "opt1!",      "Optical 1" },
  { "opt2",      "opt2!",      "Optical 2" },
  { "aux1",      "aux1!",      "AUX 1"     },
  { "aux2",      "aux2!",      "AUX 2"     },
  { "phono",     "phono!",     "Phono"     },
  { "usb",       "usb!",       "USB"       },
  { "bluetooth", "bluetooth!", "Bluetooth" },
  { "pc_usb",    "pcusb!",     "PC USB"    },
  { "bal_xlr",   "bal_xlr!",   "XLR"       },
};

// Gen-1-Geraete (RA-11/RA-12); Befehle identisch, kein PC-USB
static const RotelSourceDef ROTEL_SRC_RA12[] = {
  { "cd",        "cd!",        "CD"        },
  { "coax1",     "coax1!",     "Coax 1"    },
  { "coax2",     "coax2!",     "Coax 2"    },
  { "opt1",      "opt1!",      "Optical 1" },
  { "opt2",      "opt2!",      "Optical 2" },
  { "aux1",      "aux1!",      "AUX 1"     },
  { "aux2",      "aux2!",      "AUX 2"     },
  { "tuner",     "tuner!",     "Tuner"     },
  { "phono",     "phono!",     "Phono"     },
  { "usb",       "usb!",       "USB"       },
};

// RA-1570 (Gen 1): PC-USB heisst im Gen-1-Dialekt "pc_usb!"
static const RotelSourceDef ROTEL_SRC_RA1570[] = {
  { "cd",        "cd!",        "CD"        },
  { "coax1",     "coax1!",     "Coax 1"    },
  { "coax2",     "coax2!",     "Coax 2"    },
  { "opt1",      "opt1!",      "Optical 1" },
  { "opt2",      "opt2!",      "Optical 2" },
  { "aux1",      "aux1!",      "AUX 1"     },
  { "aux2",      "aux2!",      "AUX 2"     },
  { "tuner",     "tuner!",     "Tuner"     },
  { "phono",     "phono!",     "Phono"     },
  { "usb",       "usb!",       "USB"       },
  { "bal_xlr",   "bal_xlr!",   "XLR"       },
  { "pc_usb",    "pc_usb!",    "PC USB"    },
};

// ---------- Modelltabelle ----------

#define ROTEL_SRC_ENTRY(list) list, (uint8_t)(sizeof(list) / sizeof(list[0]))

// Reihenfolge: A-Serie -> klassische RA-Serie (Gen 1) -> grosse RA-Serie ->
// Michi; Endstufen am Schluss. Der Index ist zugleich der NVS-Wert der
// Modellauswahl – bei Umsortierung ROTEL_DEFAULT_MODEL mit anpassen!
static constexpr RotelModelDef ROTEL_MODELS[] = {
  //  Name                        Generation       Typ              Quellen                          BalMax  Tone
  { "A11 (+MKII)",                ROTEL_GEN2,      ROTEL_FULL_AMP,  ROTEL_SRC_ENTRY(ROTEL_SRC_A11),    15,  true  },
  { "A12 / A14 (+MKII)",          ROTEL_GEN2,      ROTEL_FULL_AMP,  ROTEL_SRC_ENTRY(ROTEL_SRC_A14),    15,  true  },
  { "RA-11 / RA-12",              ROTEL_GEN1,      ROTEL_FULL_AMP,  ROTEL_SRC_ENTRY(ROTEL_SRC_RA12),   15,  true  },
  { "RA-1570",                    ROTEL_GEN1,      ROTEL_FULL_AMP,  ROTEL_SRC_ENTRY(ROTEL_SRC_RA1570), 15,  true  },
  { "RA-1572 (+MKII)",            ROTEL_GEN_AUTO,  ROTEL_FULL_AMP,  ROTEL_SRC_ENTRY(ROTEL_SRC_RA157X), 15,  true  },
  { "RA-1592 (+MKII)",            ROTEL_GEN_AUTO,  ROTEL_FULL_AMP,  ROTEL_SRC_ENTRY(ROTEL_SRC_RA157X), 15,  true  },
  { "RA-6000",                    ROTEL_GEN2,      ROTEL_FULL_AMP,  ROTEL_SRC_ENTRY(ROTEL_SRC_RA157X), 15,  true  },
  { "Michi X3 / X5",              ROTEL_GEN2,      ROTEL_FULL_AMP,  ROTEL_SRC_ENTRY(ROTEL_SRC_MICHI_X),10,  true  },
  { "Michi M8 / S5 (Endstufe)",   ROTEL_GEN2,      ROTEL_POWER_AMP, nullptr, 0,                         0,  false },
};

constexpr uint8_t ROTEL_MODEL_COUNT   = sizeof(ROTEL_MODELS) / sizeof(ROTEL_MODELS[0]);
constexpr uint8_t ROTEL_DEFAULT_MODEL = 1;    // A12/A14 (Index in ROTEL_MODELS)
constexpr uint8_t ROTEL_MAX_SOURCES   = 16;   // Obergrenze fuer statische Arrays im Sketch

// Compile-Zeit-Wache: keine Quellenliste darf die statischen Arrays im Sketch
// (inputs[ROTEL_MAX_SOURCES]) sprengen. Rekursion laeuft nur im Compiler.
constexpr bool rotelSourcesFit(uint8_t i = 0){
  return i >= ROTEL_MODEL_COUNT
      || (ROTEL_MODELS[i].sourceCount <= ROTEL_MAX_SOURCES && rotelSourcesFit((uint8_t)(i + 1)));
}
static_assert(rotelSourcesFit(), "Quellenliste eines Modells ueberschreitet ROTEL_MAX_SOURCES");

// ---------- NVS: Modellauswahl + Generation-Cache ----------
// Einmalige Lese-/Schreibzugriffe (Boot bzw. Nutzeraktion) – kein zyklisches
// Schreiben, daher flash-schonend.

inline uint8_t rotelLoadModelIndex(){
  Preferences p;
  p.begin("rotelcfg", true);
  uint8_t idx = p.getUChar("model", ROTEL_DEFAULT_MODEL);
  p.end();
  if (idx >= ROTEL_MODEL_COUNT) idx = ROTEL_DEFAULT_MODEL;
  return idx;
}

inline void rotelSaveModelIndex(uint8_t idx){
  Preferences p;
  p.begin("rotelcfg", false);
  p.putUChar("model", idx);
  p.remove("gencache");   // neue Hardware -> Erkennung neu durchlaufen
  p.end();
}

inline uint8_t rotelLoadGenCache(){
  Preferences p;
  p.begin("rotelcfg", true);
  uint8_t g = p.getUChar("gencache", ROTEL_GEN_UNKNOWN);
  p.end();
  return (g == ROTEL_GEN1 || g == ROTEL_GEN2) ? g : ROTEL_GEN_UNKNOWN;
}

inline void rotelSaveGenCache(uint8_t gen){
  Preferences p;
  p.begin("rotelcfg", false);
  p.putUChar("gencache", gen);
  p.end();
}

#endif
