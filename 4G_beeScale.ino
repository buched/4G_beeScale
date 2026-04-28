/*
 * 4G_beeScale - Système de pesée de ruche connectée
 * Copyright (C) 2026  Emmanuel COUVERCELLE (manu@couvercelle.eu)
 *
 * Ce programme est un logiciel libre : vous pouvez le redistribuer 
 * et/ou le modifier selon les termes de la Licence Publique Générale 
 * GNU telle que publiée par la Free Software Foundation, soit la 
 * version 3 de la licence, ou (à votre discrétion) toute version ultérieure.
 *
 * Ce programme est distribué dans l'espoir qu'il sera utile, mais 
 * SANS AUCUNE GARANTIE ; sans même la garantie implicite de 
 * COMMERCIALISATION ou d'ADÉQUATION À UN USAGE PARTICULIER. 
 * Voir la Licence Publique Générale GNU pour plus de détails.
 *
 * Vous devriez avoir reçu une copie de la Licence Publique Générale 
 * GNU avec ce programme. Sinon, voir <https://www.gnu.org/licenses/>.
 */
#define TINY_GSM_MODEM_A7670
#include "TinyGsmClientfork.h"
#include <HardwareSerial.h>
HardwareSerial SerialAT(1);
#include "HX711.h"
#include <rom/rtc.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
// Déclaration de la fonction interne de l'ESP32
#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read();
#ifdef __cplusplus
}
#endif
float temp_actuelle = 0.00;
// ===== CONFIGURATION DEEP SLEEP =====
#define ENABLE_DEEP_SLEEP true
const unsigned long DUREE_SLEEP_MS = 1800000;  // 30 min

// ===== CHOIX MÉTHODE D'ENVOI =====
enum MethodeEnvoi {
  METHODE_GET,      // URL avec paramètres: /postg.php?idx=xxx&poids=xxx&bat=xxx
  METHODE_POST_JSON // POST avec body JSON: {"idx":"xxx","poids":xxx,"bat":xxx}
};

//#define METHODE_ACTIVE METHODE_GET
#define METHODE_ACTIVE METHODE_POST_JSON

// ===== CONFIGURATION BATTERIE =====
#define SEUIL_BATTERIE_FAIBLE 3500 // en mv
#define SEUIL_BATTERIE_CRITIQUE 3200 // en mv

// ===== VARIABLES RTC (PERSISTENT ENTRE LES RÉVEILS) =====
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR unsigned long totalAlertes_RTC = 0;
RTC_DATA_ATTR bool alerteBatterieDejaSent = false;
RTC_DATA_ATTR unsigned long dernierSMSBatterie = 0;
RTC_DATA_ATTR long tareOffset_RTC = 0;
RTC_DATA_ATTR int dernierBootAlerte = 0;

// NOUVELLES VARIABLES RTC POUR STATISTIQUES
RTC_DATA_ATTR unsigned long totalEnvoisHTTP_RTC = 0;
RTC_DATA_ATTR unsigned long envoiHTTPReussis_RTC = 0;
RTC_DATA_ATTR unsigned long alertesSMSReussies_RTC = 0;
RTC_DATA_ATTR unsigned long alertesAppelReussies_RTC = 0;

const unsigned long DELAI_MIN_SMS_BATTERIE = 86400000;

// ===== PINS =====
#define HX_DT 19
#define HX_SCK 18
#define GPIO_WAKEUP 32
#define PIN_RX 27
#define PIN_TX 26
#define MODEM_PWRKEY 4
#define MODEM_DTR 25
#define BOARD_POWERON 12
#define BAT_ADC_PIN 35
#define HX711_WAKEUP 13

// ===== CONFIGURATION RÉSEAU Sosh=====
const char apn[] = "orange";
const char user[] = "orange";
const char pass[] = "orange";

// ===== CONFIGURATION RÉSEAU reglo mobile=====
//const char apn[] = "sl2sfr";
//const char user[] = "";
//const char pass[] = "";

// ===== CONFIGURATION SERVEUR =====
struct ServerConfig {
  const char* host = "mondomaine.fr";//domaine vers lequel envoyer
  const char* port = "8093";//port vers lequel envoyer
  const char* endpoint = "postg.php";//nom de la page appelée en methode GET
  const char* endpointjson = "postjson.php";//nom de la page appelée en methode JSON
  const char* arg1 = "idx";
  const char* arg2 = "poids";
  const char* arg3 = "bat";
  const char* arg4 = "rssi";
};
ServerConfig server;

// ===== STATISTIQUES (EN RAM, RESTAURÉES DEPUIS RTC) =====
struct Statistiques {
  unsigned long totalAlertes = 0;
  unsigned long alertesSMSReussies = 0;
  unsigned long alertesAppelReussies = 0;
  unsigned long totalEnvoisHTTP = 0;
  unsigned long envoiHTTPReussis = 0;
};
Statistiques stats;

// ===== CONFIGURATION DEVICE =====
const String DEVICE_IDX = "0606060606";// num de la carte sim utilisée par cette ruche
const String ALERT_PHONE = "+33707070707";// num vers lequel sont dirigés les sms et appels

float poids_g = 0;
float lastBatPercent = 0.0;
float lastBatVoltage = 0.0;
uint32_t volts;

// ===== VARIABLES GLOBALES =====
HX711 scale;
TinyGsm modem(SerialAT);
TinyGsmClient gsmClient(modem);

int lastState = HIGH;
bool alerteEnCours = false;

#define UART_BAUD 115200

// ===== FONCTIONS DE SAUVEGARDE/RESTAURATION STATS =====
void restaurerStats() {
  stats.totalAlertes = totalAlertes_RTC;
  stats.totalEnvoisHTTP = totalEnvoisHTTP_RTC;
  stats.envoiHTTPReussis = envoiHTTPReussis_RTC;
  stats.alertesSMSReussies = alertesSMSReussies_RTC;
  stats.alertesAppelReussies = alertesAppelReussies_RTC;
  
  Serial.println("Statistiques restaurées depuis RTC:");
  Serial.print("  Boot #");
  Serial.println(bootCount);
  Serial.print("  Total envois HTTP: ");
  Serial.println(stats.totalEnvoisHTTP);
  Serial.print("  Envois HTTP réussis: ");
  Serial.println(stats.envoiHTTPReussis);
}

void sauvegarderStats() {
  totalAlertes_RTC = stats.totalAlertes;
  totalEnvoisHTTP_RTC = stats.totalEnvoisHTTP;
  envoiHTTPReussis_RTC = stats.envoiHTTPReussis;
  alertesSMSReussies_RTC = stats.alertesSMSReussies;
  alertesAppelReussies_RTC = stats.alertesAppelReussies;
  
  Serial.println("Statistiques sauvegardées dans RTC");
}

float lireTempESP32() {
    // Lecture brute (valeur entre 0 et 255)
    // Cette fonction retourne généralement des degrés Fahrenheit 
    // ou une valeur brute à convertir.
    float brute = temprature_sens_read();
    
    // Conversion standard pour ESP32 : (Valeur - 32) / 1.8
    float celsius = (brute - 32) / 1.8;
    
    return celsius;
}

// ===== FONCTION PRINT WAKEUP REASON =====
void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();

  Serial.println("\n=== RÉVEIL ESP32 ===");
  Serial.print("Boot #");
  Serial.println(bootCount);
  
  Serial.print("Méthode d'envoi: ");
  if (METHODE_ACTIVE == METHODE_GET) {
    Serial.println("HTTP GET");
  } else {
    Serial.println("HTTP POST JSON");
  }
  
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("Cause: GPIO (Anti-vol)");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Cause: Timer (Cycle périodique)");
      break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      Serial.println("Cause: Power ON / Reset");
      break;
    default:
      Serial.println("Cause: Autre");
      break;
  }
  Serial.println("========================\n");
}

// ===== FONCTION POUR METTRE LE MODEM EN VEILLE =====
void modemSleep() {
  Serial.println("Mise en veille du modem...");
  Serial.println("Séquence hardware power OFF");
  
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(2500);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(2000);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(2500);
  
  Serial.println("  Vérification extinction...");
  
  int tentatives = 0;
  bool modemEteint = false;
  
  while (tentatives < 10) {
    if (!modem.testAT()) {
      modemEteint = true;
      break;
    }
    Serial.print(".");
    delay(500);
    tentatives++;
  }
  
  if (modemEteint) {
    Serial.println("\n Modem éteint confirmé");
  } else {
    Serial.println("\n Modem répond encore");
  }
  
  delay(1000);
}

// ===== FONCTION POUR RÉVEILLER LE MODEM =====
void modemWakeup() {
  Serial.println(" Réveil du modem...");
  Serial.println("  Séquence hardware power ON");
  
  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, HIGH);
  delay(100);
  
  pinMode(MODEM_DTR, OUTPUT);
  digitalWrite(MODEM_DTR, LOW);
  delay(100);
  
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(100);
  
  Serial.println("  Impulsion PWRKEY LOW (50ms)");
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(50);
  digitalWrite(MODEM_PWRKEY, HIGH);
  
  Serial.println("  Attente démarrage modem (10s)...");
  delay(10000);
  
  Serial.println("  Vérification communication AT...");
  int tentatives = 0;
  bool modemPret = false;
  
  while (tentatives < 20 && !modemPret) {
    if (modem.testAT()) {
      modemPret = true;
      break;
    }
    Serial.print(".");
    delay(500);
    tentatives++;
  }
  
  if (modemPret) {
    Serial.println("\n Modem réveillé - Communication AT OK");
    String info = modem.getModemInfo();
    Serial.print("  Info: ");
    Serial.println(info);
  } else {
    Serial.println("\n Modem ne répond pas encore");
  }
}

// ===== FONCTION ENTER DEEP SLEEP =====
void enterDeepSleep(unsigned long dureeSleep) {
  Serial.println("\n💤 === PRÉPARATION DEEP SLEEP ===");
  Serial.print("Durée: ");
  Serial.print(dureeSleep / 60000);
  Serial.println(" minutes");
  
  // SAUVEGARDER LES STATS AVANT LE SLEEP
  sauvegarderStats();
  
  modemSleep();
  
  esp_sleep_enable_timer_wakeup(dureeSleep * 1000);
  Serial.print("Timer: ");
  Serial.print(dureeSleep / 1000);
  Serial.println(" secondes");
  
  esp_sleep_enable_ext0_wakeup((gpio_num_t)GPIO_WAKEUP, 0);
  Serial.println("Réveil GPIO32 configuré (anti-vol)");
  
  rtc_gpio_isolate(GPIO_NUM_12);
  
  Serial.println("\ Consommation estimée:");
  Serial.println("  - ESP32 deep sleep: ~10µA");
  Serial.println("  - Modem OFF: 0µA");
  Serial.println("  - HX711 off: 0µA");
  Serial.println("  - Total: ~10µA");
  
  Serial.println("\n Entrée en deep sleep...");
  Serial.println("====================================\n");
  Serial.flush();
  
  delay(100);
  esp_deep_sleep_start();
}

// ===== FONCTIONS UTILITAIRES =====

void powerOnModem() {
  Serial.println(" Mise sous tension du modem...");
  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, HIGH);
  
  pinMode(MODEM_PWRKEY, OUTPUT);
  pinMode(MODEM_DTR, OUTPUT);
  digitalWrite(MODEM_DTR, LOW);
  
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(50);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(5000);
  
  Serial.println(" Modem alimenté");
}

void viderBuffer() {
  while (SerialAT.available()) {
    SerialAT.read();
  }
}

bool reconnecterGPRS() {
  Serial.println("\n Reconnexion GPRS...");
  modem.gprsDisconnect();
  delay(2000);
  
  if (modem.gprsConnect(apn, user, pass)) {
    Serial.println(" GPRS reconnecté");
    return true;
  }
  
  Serial.println(" Reconnexion échouée");
  return false;
}

void readBatteryPercent() {
    volts = analogReadMilliVolts(BAT_ADC_PIN);
    volts *= 2;
    lastBatVoltage = volts;
}

bool lireReponseStandard(String &response) {
  unsigned long start = millis();
  while (millis() - start < 12000L) { // Timeout global de 12s
    while (gsmClient.available()) {
      char c = (char)gsmClient.read();
      response += c;
      start = millis(); // Reset timeout tant qu'on reçoit des données
    }
    // Si on détecte la fin des headers ou le code 200, on peut valider
    if (response.indexOf("200 OK") != -1) {
      return true;
    }
    delay(10);
  }
  return false;
}

bool sendWeightGET(float poids_g, String idx, float volts, int signal) {
  if (!gsmClient.connect(server.host, atoi(server.port))) return false;

  gsmClient.print(String("GET /") + server.endpoint + "?idx=" + idx + 
                 "&poids=" + String(poids_g, 2) + 
                 "&bat=" + String(volts, 0) + " HTTP/1.1\r\n" +
                 "&rssi=" + String(signal) +
                 "Host: " + server.host + "\r\n" +
                 "Connection: close\r\n\r\n");

  String response = "";
  bool success = lireReponseStandard(response);
  gsmClient.stop();
  return success;
}

bool sendWeightPOSTJSON(float poids_g, String idx, float volts, int signal, float temp) {
  if (!gsmClient.connect(server.host, atoi(server.port))) return false;

  String body = "{\"idx\":\"" + idx + "\",\"poids\":" + String(poids_g, 2) + ",\"bat\":" + String(volts, 0) + ",\"rssi\":" + String(signal) + ",\"temp\":" + String(temp, 2) + "}";

  gsmClient.print(String("POST /") + server.endpointjson + " HTTP/1.1\r\n" +
                 "Host: " + server.host + "\r\n" +
                 "Content-Type: application/json\r\n" +
                 "Content-Length: " + String(body.length()) + "\r\n" +
                 "Connection: close\r\n\r\n");
  gsmClient.print(body);

  String response = "";
  bool success = lireReponseStandard(response);
  gsmClient.stop();
  return success;
}

// ===== FONCTION UNIFIÉE D'ENVOI (SÉCURISÉE) =====
bool sendWeight(float poids_g, String idx, float volts, int signal, float temp) {
  // On s'assure que le GPRS est bien là avant de lancer la fonction spécifique
  if (!modem.isGprsConnected()) {
      Serial.println("🔄 GPRS perdu, tentative reconnexion...");
      reconnecterGPRS();
  }
  
  if (METHODE_ACTIVE == METHODE_GET) {
    return sendWeightGET(poids_g, idx, volts, signal);
  } else {
    return sendWeightPOSTJSON(poids_g, idx, volts, signal, temp);
  }
}

// ===== STATS =====
void afficherStats() {
  Serial.println("\n === STATISTIQUES ===");
  Serial.print("  Boot #");
  Serial.println(bootCount);
  Serial.print("  Total alertes: ");
  Serial.println(stats.totalAlertes);
  Serial.print("  Total envois HTTP: ");
  Serial.println(stats.totalEnvoisHTTP);
  Serial.print("  Envois HTTP OK: ");
  Serial.print(stats.envoiHTTPReussis);
  if (stats.totalEnvoisHTTP > 0) {
    Serial.print(" (");
    Serial.print((stats.envoiHTTPReussis * 100) / stats.totalEnvoisHTTP);
    Serial.println("%)");
  } else {
    Serial.println();
  }
  
  unsigned long uptime = millis() / 1000;
  Serial.print("  Uptime: ");
  Serial.print(uptime / 3600);
  Serial.print("h ");
  Serial.print((uptime % 3600) / 60);
  Serial.println("m");
  Serial.println("========================\n");
}

// ===== FONCTIONS SMS =====

bool envoyerSMS(String numero, String message) {
  Serial.println("\n Envoi SMS...");
  Serial.print("  Destinataire: ");
  Serial.println(numero);
  Serial.print("  Message: ");
  Serial.println(message);
  
  // 1. Configurer le mode texte
  SerialAT.println("AT+CMGF=1");
  delay(500);
  
  String reponse = "";
  while (SerialAT.available()) {
    char c = SerialAT.read();
    reponse += c;
    Serial.write(c);
  }
  
  if (reponse.indexOf("OK") == -1) {
    Serial.println(" CMGF échec");
    return false;
  }
  
  // 2. Configurer l'encodage (pour accents français)
  SerialAT.println("AT+CSCS=\"GSM\"");
  delay(500);
  viderBuffer();
  
  // 3. Envoyer le numéro destinataire
  SerialAT.print("AT+CMGS=\"");
  SerialAT.print(numero);
  SerialAT.println("\"");
  delay(1000);
  
  // Attendre le prompt ">"
  reponse = "";
  unsigned long debut = millis();
  bool promptRecu = false;
  
  while (millis() - debut < 3000) {
    if (SerialAT.available()) {
      char c = SerialAT.read();
      reponse += c;
      Serial.write(c);
      
      if (reponse.indexOf(">") != -1) {
        promptRecu = true;
        break;
      }
    }
  }
  
  if (!promptRecu) {
    Serial.println(" Pas de prompt '>'");
    return false;
  }
  
  // 4. Envoyer le message
  SerialAT.print(message);
  delay(100);
  
  // 5. Envoyer Ctrl+Z pour valider (code ASCII 26)
  SerialAT.write(26);
  
  // 6. Attendre confirmation
  Serial.println("\n  Envoi en cours...");
  debut = millis();
  reponse = "";
  bool envoiReussi = false;
  
  while (millis() - debut < 10000) {  // 10 secondes max
    if (SerialAT.available()) {
      char c = SerialAT.read();
      reponse += c;
      Serial.write(c);
      
      // Rechercher +CMGS: suivi d'un numéro
      if (reponse.indexOf("+CMGS:") != -1) {
        envoiReussi = true;
        break;
      }
      
      // Ou simplement OK dans certains cas
      if (reponse.indexOf("OK") != -1 && reponse.length() > 10) {
        envoiReussi = true;
        break;
      }
      
      // Erreur CMS
      if (reponse.indexOf("+CMS ERROR:") != -1) {
        Serial.println("\n Erreur CMS");
        return false;
      }
    }
  }
  
  if (envoiReussi) {
    Serial.println("\n SMS envoyé avec succès !");
    return true;
  } else {
    Serial.println("\n Timeout envoi SMS");
    return false;
  }
}

// Fonction pour envoyer SMS avec plusieurs tentatives
bool envoyerSMSAvecRetry(String numero, String message, int tentatives = 3) {
  for (int i = 0; i < tentatives; i++) {
    Serial.print("Tentative ");
    Serial.print(i + 1);
    Serial.print("/");
    Serial.println(tentatives);
    
    if (envoyerSMS(numero, message)) {
      return true;
    }
    
    if (i < tentatives - 1) {
      Serial.println("  Nouvelle tentative dans 3s...");
      delay(3000);
    }
  }
  
  Serial.println(" Échec après toutes les tentatives");
  return false;
}

// ===== FONCTIONS TÉLÉPHONIE =====

bool appellerNumero(String numero) {
  Serial.print(" Appel vers ");
  Serial.println(numero);
  
  // Vérifier que le modem est enregistré sur le réseau
  SerialAT.println("AT+CREG?");
  if (!attendreReponse("+CREG: 0,1", 2000) && !attendreReponse("+CREG: 0,5", 2000)) {
    Serial.println(" Pas de réseau pour appeler");
    return false;
  }
  
  viderBuffer();
  
  // Composer le numéro
  SerialAT.print("ATD");
  SerialAT.print(numero);
  SerialAT.println(";");
  
  delay(2000);
  
  Serial.println(" Appel en cours...");
  return true;
}

void raccrocher() {
  Serial.println(" Raccrochage...");
  
  // Méthode 1 : AT+CHUP (recommandée pour A7670)
  SerialAT.println("AT+CHUP");
  delay(2000);  // Délai suffisant pour libérer la ligne
  
  // Méthode 2 : ATH en backup
  SerialAT.println("ATH");
  delay(1000);
  
  // Vider les réponses
  while (SerialAT.available()) {
    Serial.write(SerialAT.read());
  }
  
  Serial.println(" Raccrochage effectué");
}


bool attendreReponse(const char* reponseAttendue, unsigned long timeout) {
  String buffer = "";
  unsigned long debut = millis();
  
  while (millis() - debut < timeout) {
    if (SerialAT.available()) {
      char c = SerialAT.read();
      buffer += c;
      Serial.write(c);  // Echo pour debug
      
      // Chercher la réponse attendue PARTOUT dans le buffer
      if (buffer.indexOf(reponseAttendue) != -1) {
        return true;
      }
      
      // Vider buffer si trop grand (éviter débordement mémoire)
      if (buffer.length() > 500) {
        buffer = buffer.substring(250);  // Garder les 250 derniers caractères
      }
    }
  }
  
  // Timeout
  Serial.print("\n[TIMEOUT - Cherchait: ");
  Serial.print(reponseAttendue);
  Serial.println("]");
  return false;
}

void deepSleepSurvie() {
    Serial.println(" !!! TENSION CRITIQUE !!!");
    Serial.println(" Entrée en mode survie (Sommeil profond 24h)");
    
    // 1. On éteint tout le matériel
    digitalWrite(HX711_WAKEUP, LOW);
    modemSleep();
    
    // 2. On désactive les réveils inutiles (on ne veut même plus l'antivol pour économiser)
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    
    // 3. On demande un réveil dans 24h pour vérifier si on a été rechargé
    // (86400 secondes * 1 000 000 pour les microsecondes)
    esp_sleep_enable_timer_wakeup(86400ULL * 1000000ULL);
    
    // 4. On s'endort
    esp_deep_sleep_start();
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.print(" Test Température ESP32 : ");
//Serial.println(lireTempESP32());
  temp_actuelle = lireTempESP32();
  pinMode(GPIO_WAKEUP, INPUT_PULLUP);
  delay(200);
  ++bootCount;
  
  // RESTAURER LES STATS DEPUIS RTC MEMORY
  if (bootCount > 1) {
    restaurerStats();
  }
  
  Serial.println("\n\n=================================");
  Serial.println("   LILYGO A7670G + HX711");
  Serial.println("   Mode Deep Sleep Activé");
  Serial.println("=================================\n");
  
  print_wakeup_reason();
  
  SerialAT.begin(UART_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

  pinMode(HX711_WAKEUP, OUTPUT);
  delay(200);
  digitalWrite(HX711_WAKEUP, HIGH);
  delay(1000);

//ajout sms antivol
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool reveilParAlerte = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);
  if (digitalRead(GPIO_WAKEUP) == HIGH)
    {
      Serial.println(" Faux réveil EXT0 détecté (parasite). Annulation.");
    }
  else
    {
      if (reveilParAlerte) {
        Serial.println(" RÉVEIL PAR ALERTE ANTI-VOL !");
        readBatteryPercent();
        // Démarrage modem
        powerOnModem();
        delay(3000);
        
        if (!modem.restart()) {
          Serial.println(" modem.restart() échoué");
        }
        
        if (!modem.gprsConnect(apn, user, pass)) {
          Serial.println(" GPRS échoué");
        } else {
          Serial.println(" GPRS connecté");
        }
        
        // Traiter l'alerte immédiatement
        stats.totalAlertes++;
        
        // Init rapide HX711
        scale.begin(HX_DT, HX_SCK);
        scale.set_scale(17.2f);
        delay(100);
        scale.set_offset(tareOffset_RTC);
        poids_g = scale.get_units(5);
              if (poids_g < 0)
                {
                  poids_g = 0;
                }
        String messageAlerte = "ALERTE BALANCE!\n";
        messageAlerte += "Mouvement detecte\n";
        messageAlerte += "Poids: " + String(poids_g, 1) + "g\n";
        messageAlerte += "Bat: " + String(volts) + "mV\n"; // Ajout de la batterie
        messageAlerte += "ID: " + DEVICE_IDX;
        
        if (envoyerSMSAvecRetry(ALERT_PHONE, messageAlerte, 2)) {
          Serial.println(" SMS envoyé");
          stats.alertesSMSReussies++;
        }
        
        delay(2000);
        
        if (appellerNumero(ALERT_PHONE)) {
          stats.alertesAppelReussies++;
          delay(20000);
          raccrocher();
        }
        
        Serial.println("\n Alerte traitée");
        digitalWrite(HX711_WAKEUP, LOW);
        delay(2000);
        // Retourner en sleep immédiatement
        enterDeepSleep(DUREE_SLEEP_MS);
      }
    }

  //fin ajout antivol
  if (bootCount == 1) {
    Serial.println(" PREMIER BOOT - Initialisation complète du modem");
    powerOnModem();
    delay(3000);
    
    if (!modem.restart()) {
      Serial.println(" modem.restart() échoué");
      enterDeepSleep(60000);
    }
  } else {
    Serial.println(" RÉVEIL - Wakeup du modem depuis sleep");
    modemWakeup();
  }
  
  Serial.println(" Cycle d'envoi de données");
  Serial.println(" Vérification modem...");
  
  int tentativesAT = 0;
  bool modemPret = false;
  
  while (tentativesAT < 10 && !modemPret) {
    if (modem.testAT()) {
      modemPret = true;
      Serial.println(" Modem répond");
      break;
    }
    Serial.print(".");
    delay(500);
    tentativesAT++;
  }
  
  if (!modemPret) {
    Serial.println("\n Modem ne répond pas, tentative restart...");
    if (!modem.restart()) {
      Serial.println(" modem.restart() échoué");
      enterDeepSleep(60000);
    }
    delay(3000);
  }
  
  Serial.println("📡 Attente réseau...");
  if (!modem.waitForNetwork(60000L)) {
    Serial.println(" Pas de réseau");
    enterDeepSleep(60000);
  }
  
  Serial.println(" Réseau OK");
  
  String operateur = modem.getOperator();
  Serial.print("📡 Opérateur: ");
  Serial.println(operateur);
  
  int signal = modem.getSignalQuality();
  Serial.print(" Signal: ");
  Serial.print(signal);
  Serial.println("/31");
  
  Serial.println("📡 Connexion GPRS...");
  
  if (modem.isGprsConnected()) {
    Serial.println(" GPRS déjà connecté");
  } else {
    modem.gprsDisconnect();
    delay(1000);
    
    if (!modem.gprsConnect(apn, user, pass)) {
      Serial.println(" GPRS échoué - Activation manuelle PDP...");
      
      SerialAT.print("AT+CGDCONT=1,\"IP\",\"");
      SerialAT.print(apn);
      SerialAT.println("\"");
      delay(1000);
      
      SerialAT.println("AT+CGACT=1,1");
      delay(2000);
      
      SerialAT.println("AT+CGATT=1");
      delay(2000);
      
      if (!modem.gprsConnect(apn, user, pass)) {
        Serial.println(" GPRS échoué définitivement");
        enterDeepSleep(60000);
      }
    }
  }

  Serial.println(" GPRS connecté");
  Serial.print(" IP : ");
  Serial.println(modem.localIP());

  Serial.println(" Initialisation HX711...");
  scale.begin(HX_DT, HX_SCK);
  scale.set_scale(17.2f);
  delay(100);

if (bootCount == 1) {
    Serial.println(" PREMIER BOOT - Calcul de la tare initiale...");
    scale.tare(20); // Calcule la tare avec 20 mesures
    tareOffset_RTC = scale.get_offset(); // On mémorise la valeur brute du zéro
    Serial.print(" Offset sauvegardé en RTC : ");
    Serial.println(tareOffset_RTC);
  } else {
    Serial.println(" RÉVEIL - Restauration de la tare depuis RTC");
    scale.set_offset(tareOffset_RTC); // On réapplique le zéro mémorisé au premier boot
    Serial.print(" Offset restauré : ");
    Serial.println(tareOffset_RTC);
  }

  Serial.println(" Système prêt !");
  Serial.println("=================================\n");
}

// ===== LOOP =====
void loop() {
  Serial.println("\n === CYCLE D'ENVOI ===");
  
  readBatteryPercent();
  // --- SÉCURITÉ BATTERIE CRITIQUE ---
    if (volts < SEUIL_BATTERIE_CRITIQUE) {
        // Optionnel : Envoyer un dernier SMS de détresse si on ne l'a pas encore fait
        if (!alerteBatterieDejaSent) {
             envoyerSMSAvecRetry(ALERT_PHONE, "BATTERIE CRITIQUE (" + String(volts) + "mV). Arret du systeme.", 1);
             alerteBatterieDejaSent = true;
        }
        deepSleepSurvie(); 
    }
    
  // --- GESTION ALERTE BATTERIE FAIBLE ---
    if (volts < SEUIL_BATTERIE_FAIBLE) {
    // On envoie si :
    // 1. C'est la toute première fois (alerteBatterieDejaSent est false)
    // 2. OU si 48 cycles se sont écoulés depuis le dernier SMS (~24h)
    if (!alerteBatterieDejaSent || (bootCount - dernierBootAlerte >= 48)) {
        
        Serial.println(" Seuil batterie faible atteint. Préparation SMS...");
        
        // On s'assure que le modem est réveillé (normalement il l'est déjà dans votre setup)

        if (envoyerSMSAvecRetry(ALERT_PHONE, " Tension Batterie basse (" + String(volts) + "mV). Vérifiez ou rechargez.", 1)) {
            alerteBatterieDejaSent = true;
            dernierBootAlerte = bootCount; // On mémorise quand on a envoyé
            Serial.println(" SMS alerte batterie envoyé.");
        }
    }
} else {
    // Si la tension remonte (recharge solaire par ex), on réinitialise pour la prochaine fois
    if (volts > (SEUIL_BATTERIE_FAIBLE + 100)) { 
        alerteBatterieDejaSent = false;
    }
}

  if (!modem.isGprsConnected()) {
    Serial.println(" GPRS déconnecté");
    if (!reconnecterGPRS()) {
      Serial.println(" Impossible de reconnecter");
      enterDeepSleep(DUREE_SLEEP_MS);
    }
  }
  
  if (scale.wait_ready_timeout(1000)) {
    poids_g = scale.get_units(10);

    if (poids_g < 0) poids_g = 0;
    
    Serial.print("  Poids : ");
    Serial.print(poids_g, 2);
    Serial.println(" g");
    
    stats.totalEnvoisHTTP++;
    int signal_rssi = modem.getSignalQuality();

    // Si le signal est inconnu (99), on fait une deuxième tentative après 500ms
    if (signal_rssi == 99)
      {
          delay(500);
          signal_rssi = modem.getSignalQuality();
      }

    // Si c'est toujours 99, on peut décider de mettre 0 ou une valeur par défaut
    // pour ne pas fausser vos statistiques
    if (signal_rssi == 99) signal_rssi = 0;

    bool envoiReussi = false;
         if (poids_g < 0)
            {
              poids_g = 0;
            }
    for (int tentative = 0; tentative < 3; tentative++) {
      if (sendWeight(poids_g, DEVICE_IDX, volts, signal_rssi, temp_actuelle)) {
        Serial.println(" ENVOI OK");
        Serial.println(temp_actuelle);
        stats.envoiHTTPReussis++;
        envoiReussi = true;
        break;
      } else {
        Serial.print(" ENVOI ECHEC (");
        Serial.print(tentative + 1);
        Serial.println("/3)");
        
        if (tentative < 2) {
          if (!modem.isGprsConnected()) {
            reconnecterGPRS();
          }
          delay(2000);
        }
      }
    }
    
    if (!envoiReussi) {
      Serial.println(" ÉCHEC DÉFINITIF");
    }
    
    afficherStats();
    
  } else {
    Serial.println(" HX711 timeout");
  }
  
  Serial.println("========================\n");
  digitalWrite(HX711_WAKEUP, LOW);
  delay(2000);
  
  if (ENABLE_DEEP_SLEEP) {
    enterDeepSleep(DUREE_SLEEP_MS);
  } else {
    Serial.println(" Deep sleep désactivé (mode debug)");
    delay(DUREE_SLEEP_MS);
  }
}
