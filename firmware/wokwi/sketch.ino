/**
 * MbënnScan — Simulation Balise ESP32 sur Wokwi
 *
 * Pour tester directement sur https://wokwi.com/projects/new/esp32
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ── Configuration Wokwi & Supabase ──────────────────────────────────────────
#define WIFI_SSID          "Wokwi-GUEST"
#define WIFI_PASSWORD      ""

#define MESURE_URL         "https://xzfxihccjcosujynptwh.supabase.co/functions/v1/mesure"
#define BALISE_API_KEY     "mbenn_Dr77VVXVAaVsCANpZO70vcDDpWPnwB3Xa5-mCW82QdE"
#define BALISE_ID          "bal-01"
#define MESURE_INTERVALLE_MS 5000

// ── Broches matérielles ──────────────────────────────────────────────────────
#define PIN_TRIG 5
#define PIN_ECHO 18
#define PIN_LED  2

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

  int taille() const { return compte; }
  bool estVide() const { return compte == 0; }
};

static MesureBuffer bufferSecours;

enum EtatSysteme { ETAT_OK, ETAT_ERREUR_RESEAU };
static EtatSysteme etatActuel = ETAT_ERREUR_RESEAU;
static unsigned long dernierTempsMesure = 0;
static unsigned long dernierTempsLed = 0;
static unsigned long dernierTempsEnvoi = 0;
static bool etatLed = false;

void actualiserLed() {
  unsigned long now = millis();
  unsigned long periode = (etatActuel == ETAT_OK) ? 1000 : 200;

  if (now - dernierTempsLed >= (periode / 2)) {
    dernierTempsLed = now;
    etatLed = !etatLed;
    digitalWrite(PIN_LED, etatLed ? HIGH : LOW);
  }
}

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

float lireDistanceBrute() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  long duree_us = pulseIn(PIN_ECHO, HIGH, 35000);
  if (duree_us <= 0) return -1.0f;

  return (duree_us * 0.0343f) / 2.0f;
}

bool mesurerDistanceFiltree(float &distanceFinale) {
  float lectures[5];
  int lecturesValides = 0;

  for (int i = 0; i < 5; i++) {
    float d = lireDistanceBrute();
    if (d >= 20.0f && d <= 600.0f) {
      lectures[lecturesValides++] = d;
    }
    delay(60);
  }

  if (lecturesValides < 3) {
    Serial.println("[Capteur] Écho insuffisant ou valeur hors limites [20, 600] cm");
    return false;
  }

  for (int i = 0; i < lecturesValides - 1; i++) {
    for (int j = i + 1; j < lecturesValides; j++) {
      if (lectures[j] < lectures[i]) {
        float temp = lectures[i];
        lectures[i] = lectures[j];
        lectures[j] = temp;
      }
    }
  }

  distanceFinale = lectures[lecturesValides / 2];
  return true;
}

bool envoyerMesure(const MesureRecord& m) {
  if (WiFi.status() != WL_CONNECTED) return false;

  unsigned long now = millis();
  if (now - dernierTempsEnvoi < 1100) {
    delay(1100 - (now - dernierTempsEnvoi));
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, MESURE_URL)) {
    Serial.println("[HTTP] Échec d'initialisation");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  String auth = String("Bearer ") + BALISE_API_KEY;
  http.addHeader("Authorization", auth.c_str());

  JsonDocument doc;
  doc["balise_id"] = BALISE_ID;
  doc["distance_cm"] = serialized(String(m.distance_cm, 1));
  doc["batterie_v"] = nullptr;
  doc["rssi"] = m.rssi;

  String corpsJson;
  serializeJson(doc, corpsJson);

  Serial.print("[HTTP] Envoi vers Supabase : ");
  Serial.println(corpsJson);

  int codeReponse = http.POST(corpsJson);
  dernierTempsEnvoi = millis();

  Serial.print("[HTTP] Code retour : ");
  Serial.println(codeReponse);

  bool succes = (codeReponse == 201);
  if (!succes) {
    String reponse = http.getString();
    Serial.print("[HTTP] Réponse détaillée : ");
    Serial.println(reponse);
  }

  http.end();
  return succes;
}

void viderBufferSecours() {
  if (bufferSecours.estVide() || WiFi.status() != WL_CONNECTED) return;

  Serial.print("[Buffer] Vidage de ");
  Serial.print(bufferSecours.taille());
  Serial.println(" mesure(s) en attente...");

  MesureRecord m;
  while (bufferSecours.depiler(m)) {
    actualiserLed();
    if (!envoyerMesure(m)) {
      bufferSecours.empiler(m);
      Serial.println("[Buffer] Échec du renvoi");
      etatActuel = ETAT_ERREUR_RESEAU;
      return;
    }
  }
  Serial.println("[Buffer] Toutes les mesures en attente ont été envoyées !");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== MbënnScan — Simulation Wokwi ESP32 ===");

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  connecterWiFi();
}

void loop() {
  actualiserLed();

  if (WiFi.status() != WL_CONNECTED) {
    etatActuel = ETAT_ERREUR_RESEAU;
    static unsigned long dernierEssaiWiFi = 0;
    if (millis() - dernierEssaiWiFi > 10000) {
      dernierEssaiWiFi = millis();
      connecterWiFi();
    }
  }

  unsigned long now = millis();
  if (now - dernierTempsMesure >= MESURE_INTERVALLE_MS) {
    dernierTempsMesure = now;

    float distance = 0.0f;
    if (mesurerDistanceFiltree(distance)) {
      MesureRecord record;
      record.distance_cm = distance;
      record.rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -60;
      record.a_batterie = false;

      Serial.print("[Capteur] Distance mesurée : ");
      Serial.print(distance, 1);
      Serial.println(" cm");

      bool ok = false;
      if (WiFi.status() == WL_CONNECTED) {
        ok = envoyerMesure(record);
      }

      if (ok) {
        etatActuel = ETAT_OK;
        viderBufferSecours();
      } else {
        Serial.println("[Buffer] Échec envoi -> stockage tampon");
        bufferSecours.empiler(record);
        etatActuel = ETAT_ERREUR_RESEAU;
      }
    }
  }
}
