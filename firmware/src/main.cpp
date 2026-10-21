/**
 * MbënnScan — Firmware Balise de Niveau d'Eau (ESP32)
 *
 * Prototype pour le hackathon SENSAT-TRL « Du satellite à la solution ».
 * Responsable : Seydina Aboubacry KANE
 *
 * Fonctionnalités :
 * - Mesure de distance par capteur à ultrasons étanche (JSN-SR04T / HC-SR04).
 * - Filtre médian sur 5 lectures consécutives, rejet des valeurs hors [20 cm, 600 cm].
 * - Envoi HTTPS POST vers Supabase Edge Function (/functions/v1/mesure) au format JSON.
 * - Authentification par clé de balise (Bearer token).
 * - Buffer de secours en RAM (jusqu'à 50 mesures) avec renvoi progressif en cas de coupure.
 * - Indicateur LED d'état (clignotement lent = OK, clignotement rapide = erreur).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// Configuration des secrets (Wi-Fi, URL, identifiant et clé API)
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #include "secrets.example.h"
#endif

// ── Broches matérielles (ajustables selon câblage) ───────────────────────────
#ifndef PIN_TRIG
  #define PIN_TRIG 5
#endif

#ifndef PIN_ECHO
  #define PIN_ECHO 18
#endif

#ifndef PIN_LED
  #define PIN_LED 2    // LED intégrée sur ESP32 DevKit
#endif

#ifndef PIN_BATTERIE
  #define PIN_BATTERIE 34  // Broche ADC pour pont diviseur batterie (optionnel)
#endif

// ── Paramètres du buffer de secours ──────────────────────────────────────────
#define CAPACITE_BUFFER 50

struct MesureRecord {
  float distance_cm;
  float batterie_v;
  bool a_batterie;
  int rssi;
};

class MesureBuffer {
private:
  MesureRecord elements[CAPACITE_BUFFER];
  int tete = 0;
  int queue = 0;
  int compte = 0;

public:
  bool empiler(const MesureRecord& m) {
    if (compte == CAPACITE_BUFFER) {
      // Buffer plein : on écrase la plus ancienne mesure
      tete = (tete + 1) % CAPACITE_BUFFER;
      compte--;
    }
    elements[queue] = m;
    queue = (queue + 1) % CAPACITE_BUFFER;
    compte++;
    return true;
  }

  bool depiler(MesureRecord& m) {
    if (compte == 0) return false;
    m = elements[tete];
    tete = (tete + 1) % CAPACITE_BUFFER;
    compte--;
    return true;
  }

  int taille() const {
    return compte;
  }

  bool estVide() const {
    return compte == 0;
  }
};

static MesureBuffer bufferSecours;

// ── État global ──────────────────────────────────────────────────────────────
enum EtatSysteme {
  ETAT_OK,
  ETAT_ERREUR_RESEAU
};

static EtatSysteme etatActuel = ETAT_ERREUR_RESEAU;
static unsigned long dernierTempsMesure = 0;
static unsigned long dernierTempsLed = 0;
static unsigned long dernierTempsEnvoi = 0;
static bool etatLed = false;

// ── Gestion de la LED d'état (non-bloquante) ─────────────────────────────────
void actualiserLed() {
  unsigned long now = millis();
  unsigned long periode = (etatActuel == ETAT_OK) ? 1000 : 200; // 1 Hz (OK) vs 5 Hz (erreur)

  if (now - dernierTempsLed >= (periode / 2)) {
    dernierTempsLed = now;
    etatLed = !etatLed;
    digitalWrite(PIN_LED, etatLed ? HIGH : LOW);
  }
}

// ── Gestion de la connexion Wi-Fi ────────────────────────────────────────────
void connecterWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("[Wi-Fi] Connexion à ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long debut = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - debut) < 8000) {
    delay(250);
    Serial.print(".");
    digitalWrite(PIN_LED, !digitalRead(PIN_LED));
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[Wi-Fi] Connecté ! IP : ");
    Serial.println(WiFi.localIP());
    etatActuel = ETAT_OK;
  } else {
    Serial.println("[Wi-Fi] Échec de connexion (sera réessayé)");
    etatActuel = ETAT_ERREUR_RESEAU;
  }
}

// ── Lecture ultrasonique unitaire (en cm) ────────────────────────────────────
float lireDistanceBrute() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  // Timeout de 35 ms (~6 m max)
  long duree_us = pulseIn(PIN_ECHO, HIGH, 35000);
  if (duree_us <= 0) {
    return -1.0f;
  }

  // Vitesse du son dans l'air : ~343 m/s = 0.0343 cm/µs (aller-retour => diviser par 2)
  float distance = (duree_us * 0.0343f) / 2.0f;
  return distance;
}

// ── Filtrage par médiane sur 5 lectures ──────────────────────────────────────
bool mesurerDistanceFiltree(float &distanceFinale) {
  float lectures[5];
  int lecturesValides = 0;

  for (int i = 0; i < 5; i++) {
    float d = lireDistanceBrute();
    if (d >= 20.0f && d <= 600.0f) {
      lectures[lecturesValides++] = d;
    }
    delay(60); // Petit délai entre pings pour dissiper les échos
  }

  if (lecturesValides < 3) {
    Serial.println("[Capteur] Écho insuffisant ou valeur hors plage [20, 600] cm");
    return false;
  }

  // Tri par sélection des lectures valides
  for (int i = 0; i < lecturesValides - 1; i++) {
    for (int j = i + 1; j < lecturesValides; j++) {
      if (lectures[j] < lectures[i]) {
        float temp = lectures[i];
        lectures[i] = lectures[j];
        lectures[j] = temp;
      }
    }
  }

  // Médiane
  distanceFinale = lectures[lecturesValides / 2];
  return true;
}

// ── Lecture tension de batterie (optionnelle) ────────────────────────────────
bool lireTensionBatterie(float &tensionV) {
  #ifdef PIN_BATTERIE
    int adc = analogRead(PIN_BATTERIE);
    if (adc <= 100) {
      return false; // Pont diviseur non branché
    }
    // Formule pour pont diviseur R1=R2 (x2) avec référence ADC 3.3V sur 4095
    float tensionMesuree = (adc / 4095.0f) * 3.3f * 2.0f;
    if (tensionMesuree >= 2.5f && tensionMesuree <= 5.5f) {
      tensionV = tensionMesuree;
      return true;
    }
  #endif
  return false;
}

// ── Envoi HTTPS vers l'Edge Function Supabase ─────────────────────────────────
bool envoyerMesure(const MesureRecord& m) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  // Respect du rate limit de l'Edge Function (1 mesure / seconde)
  unsigned long now = millis();
  if (now - dernierTempsEnvoi < 1100) {
    delay(1100 - (now - dernierTempsEnvoi));
  }

  WiFiClientSecure client;
  client.setInsecure(); // Accepte certificat SSL pour la démo prototype

  HTTPClient http;
  if (!http.begin(client, MESURE_URL)) {
    Serial.println("[HTTP] Échec d'initialisation de la requête");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  String auth = String("Bearer ") + BALISE_API_KEY;
  http.addHeader("Authorization", auth.c_str());

  // Construction du corps JSON conforme au contrat mesure_entrante.schema.json
  JsonDocument doc;
  doc["balise_id"] = BALISE_ID;
  doc["distance_cm"] = serialized(String(m.distance_cm, 1));

  if (m.a_batterie) {
    doc["batterie_v"] = serialized(String(m.batterie_v, 2));
  } else {
    doc["batterie_v"] = nullptr;
  }

  if (m.rssi != 0) {
    doc["rssi"] = m.rssi;
  } else {
    doc["rssi"] = nullptr;
  }

  String corpsJson;
  serializeJson(doc, corpsJson);

  Serial.print("[HTTP] Envoi POST vers ");
  Serial.print(MESURE_URL);
  Serial.print(" : ");
  Serial.println(corpsJson);

  int codeReponse = http.POST(corpsJson);
  dernierTempsEnvoi = millis();

  Serial.print("[HTTP] Réponse code : ");
  Serial.println(codeReponse);

  bool succes = (codeReponse == 201);
  if (!succes) {
    String reponse = http.getString();
    Serial.print("[HTTP] Erreur détaillée : ");
    Serial.println(reponse);
  }

  http.end();
  return succes;
}

// ── Vidage progressif du buffer de secours ────────────────────────────────────
void viderBufferSecours() {
  if (bufferSecours.estVide() || WiFi.status() != WL_CONNECTED) return;

  Serial.print("[Buffer] Vidage de ");
  Serial.print(bufferSecours.taille());
  Serial.println(" mesure(s) en attente...");

  MesureRecord m;
  while (bufferSecours.depiler(m)) {
    actualiserLed();
    if (!envoyerMesure(m)) {
      // Échec de renvoi : réempiler et abandonner pour ce tour
      bufferSecours.empiler(m);
      Serial.println("[Buffer] Échec du renvoi, attente du prochain cycle");
      etatActuel = ETAT_ERREUR_RESEAU;
      return;
    }
  }
  Serial.println("[Buffer] Toutes les mesures en attente ont été transmises !");
}

// ── Setup & Loop ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=============================================");
  Serial.println("  MbënnScan — Balise ESP32 Initialisation   ");
  Serial.println("=============================================");

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  connecterWiFi();
}

void loop() {
  actualiserLed();

  // Reconnexion Wi-Fi si nécessaire
  if (WiFi.status() != WL_CONNECTED) {
    etatActuel = ETAT_ERREUR_RESEAU;
    static unsigned long dernierEssaiWiFi = 0;
    if (millis() - dernierEssaiWiFi > 10000) {
      dernierEssaiWiFi = millis();
      connecterWiFi();
    }
  }

  // Tâche principale de mesure selon MESURE_INTERVALLE_MS
  unsigned long now = millis();
  if (now - dernierTempsMesure >= MESURE_INTERVALLE_MS) {
    dernierTempsMesure = now;

    float distance = 0.0f;
    if (mesurerDistanceFiltree(distance)) {
      MesureRecord record;
      record.distance_cm = distance;
      record.rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
      record.a_batterie = lireTensionBatterie(record.batterie_v);

      Serial.print("[Mesure] Distance validée : ");
      Serial.print(distance, 1);
      Serial.println(" cm");

      bool ok = false;
      if (WiFi.status() == WL_CONNECTED) {
        ok = envoyerMesure(record);
      }

      if (ok) {
        etatActuel = ETAT_OK;
        // Si l'envoi a réussi, on en profite pour dépiler les anciennes mesures éventuelles
        viderBufferSecours();
      } else {
        Serial.println("[Attention] Échec de l'envoi, stockage dans le buffer de secours");
        bufferSecours.empiler(record);
        etatActuel = ETAT_ERREUR_RESEAU;
      }
    } else {
      Serial.println("[Avertissement] Mesure ignorée (bruit ou hors limites [20, 600] cm)");
    }
  }
}
