// ===============================
// Montage ESP32 - Wokwi
// Projet : éclairage automatique et économique
// Parties B + C + D + E - version structurée en fonctions
//
// Datastreams Blynk :
//   V0 = Luminosité (%)        [envoi]
//   V1 = Présence PIR (0/1)    [envoi]
//   V2 = Intensité manuelle    [réception slider 0-100]
//   V3 = Mode Auto (0/1)       [réception switch]
//   V4 = Durée éclairage (s)   [envoi]
//   V5 = Consommation (Wh)     [envoi]
//   Event "mode_manuel_long"   [notification > 2h en manuel]
// ===============================

// --- Identifiants Blynk ---
#define BLYNK_TEMPLATE_ID "TMPL25AkyXb8n"
#define BLYNK_TEMPLATE_NAME "Eclairage intelligent ESP32"
#define BLYNK_AUTH_TOKEN "32UOq-W3_q4k9xS4nkThEj_2bXdNy_L0"

#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>

// WiFi spécial Wokwi (réseau ouvert avec accès Internet)
char ssid[] = "Wokwi-GUEST";
char pass[] = "";

// Pins utilisées
#define PIR_PIN        14
#define LDR_PIN        34
#define BUTTON_PIN     13
#define LED_MAIN_PIN   26
#define LED_GREEN_PIN  27
#define BUZZER_PIN     12

// PWM LED principale
#define PWM_FREQ       5000
#define PWM_RESOLUTION 8   // 8 bits : 0 à 255

// Réglages
int seuilObscurite = 1500;
bool modeAuto = true;
int intensiteManuelle = 50;   // valeur du slider V2 (0-100 %)

// Mode nuit (heure simulée avec millis)
const unsigned long dureeHeureSimulee = 5000;  // 5 s reelles = 1 heure simulee (demo)
const int heureDepart = 20;                     // l'heure simulee demarre a 20h
const int intensiteNuit = 80;                   // intensite max reduite la nuit

// Valeurs capteurs (globales pour le partage entre fonctions)
int pirState = LOW;
int ldrValue = 0;
int luminositePct = 0;
bool obscurite = false;

// Anti-rebond bouton
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 250;

// Buzzer non bloquant
bool buzzerActif = false;
unsigned long buzzerDebut = 0;
const unsigned long buzzerDuree = 150;

// Temporisation d'extinction
unsigned long derniereDetection = 0;
bool detectionActive = false;
const unsigned long tempoExtinction = 30000;  // 30 s

// Mesure du temps de réponse PIR -> LED
int lastPirState = LOW;
unsigned long instantDetection = 0;
bool mesureEnCours = false;

// Durée d'éclairage cumulée
unsigned long dureeEclairageMs = 0;
bool ledEstAllumee = false;
unsigned long instantAllumage = 0;

// Consommation électrique simulée
const float LED_PUISSANCE_W = 5.0;   // puissance nominale supposée de la LED (W)
int intensiteActuelle = 0;           // niveau PWM courant (0-255)
float energieWh = 0.0;               // énergie cumulée (Wh)
unsigned long dernierCalculConso = 0;

// Alerte mode manuel prolongé
unsigned long debutModeManuel = 0;
bool alerteManuelEnvoyee = false;
const unsigned long seuilManuel = 20000UL;  //7200000UL    2 h en ms

// Affichage série
unsigned long lastPrintTime = 0;

BlynkTimer timer;

// =========================================================
// Réception du slider d'intensité manuelle (V2)
BLYNK_WRITE(V2) {
  intensiteManuelle = param.asInt();   // 0 à 100
}

// Réception du switch de mode (V3 : 1 = auto, 0 = manuel)
BLYNK_WRITE(V3) {
  appliquerMode(param.asInt() == 1);
}

// =========================================================
void setup() {
  Serial.begin(115200);

  pinMode(PIR_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  ledcAttach(LED_MAIN_PIN, PWM_FREQ, PWM_RESOLUTION);

  Serial.println("===== DEMARRAGE ESP32 =====");
  testMateriel();

  Serial.println("Connexion a Blynk...");
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  // Envois périodiques (1 fois par seconde)
  dernierCalculConso = millis();
  timer.setInterval(1000L, envoyerVersBlynk);
  timer.setInterval(1000L, calculerConsommation);

  Serial.println("Systeme pret.");
}

// =========================================================
void loop() {
  Blynk.run();           // communication Blynk (non bloquant)
  timer.run();           // envois programmés
  gererBuzzer();         // coupe le bip si la durée est écoulée

  lireCapteurs();        // PIR, LDR, luminosité, front montant
  gererBouton();         // bascule auto/manuel via le bouton physique
  gererEclairage();      // temporisation + commande de la LED
  verifierAlerteManuel(); // alerte mode manuel prolongé
  afficherDonnees();     // affichage série chaque seconde
}

// =========================================================
// Lecture des capteurs et détection du front montant du PIR
void lireCapteurs() {
  pirState = digitalRead(PIR_PIN);
  ldrValue = analogRead(LDR_PIN);
  luminositePct = map(ldrValue, 0, 4095, 0, 100);
  obscurite = (ldrValue < seuilObscurite);  // valeur faible = sombre

  if (pirState == HIGH && lastPirState == LOW) {
    instantDetection = millis();
    mesureEnCours = true;
  }
  lastPirState = pirState;
}

// Gestion du bouton physique (bascule auto / manuel)
void gererBouton() {
  int buttonState = digitalRead(BUTTON_PIN);
  if (buttonState == LOW && lastButtonState == HIGH &&
      millis() - lastDebounceTime > debounceDelay) {
    appliquerMode(!modeAuto);
    Blynk.virtualWrite(V3, modeAuto ? 1 : 0);  // synchronise le switch dans l'app
    lastDebounceTime = millis();
    demarrerBip(1200);

    Serial.print("Mode change : ");
    Serial.println(modeAuto ? "AUTOMATIQUE" : "MANUEL");
  }
  lastButtonState = buttonState;
}

// Cœur de la logique : temporisation + commande de la LED principale
void gererEclairage() {
  // LED verte = indicateur mode automatique
  digitalWrite(LED_GREEN_PIN, modeAuto ? HIGH : LOW);

  // Temporisation : mémorise la dernière détection
  if (pirState == HIGH) {
    derniereDetection = millis();
    detectionActive = true;
  }
  if (detectionActive && (millis() - derniereDetection >= tempoExtinction)) {
    detectionActive = false;
  }

  if (modeAuto) {
    if (detectionActive && obscurite) {
      int intensiteMax = estModeNuit() ? intensiteNuit : 255;  // tamise la nuit
      commanderLed(intensiteMax);   // LED ON

      if (mesureEnCours) {
        Serial.print(">> Temps de reponse PIR -> LED : ");
        Serial.print(millis() - instantDetection);
        Serial.println(" ms");
        mesureEnCours = false;
      }
    } else {
      commanderLed(0);     // LED OFF
    }
  } else {
    // Mode manuel : intensité réglée par le slider, indépendante des capteurs
    commanderLed(map(intensiteManuelle, 0, 100, 0, 255));
  }
}

// Alerte : mode manuel depuis trop longtemps
void verifierAlerteManuel() {
  if (!modeAuto && !alerteManuelEnvoyee && (millis() - debutModeManuel >= seuilManuel)) {
    Blynk.logEvent("mode_manuel_long");
    alerteManuelEnvoyee = true;
    Serial.println("ALERTE : mode manuel depuis plus de 2h !");
  }
}

// Affichage série chaque seconde
void afficherDonnees() {
  if (millis() - lastPrintTime >= 1000) {
    lastPrintTime = millis();
    Serial.println("-----------------------------");
    Serial.print("PIR : ");
    Serial.println(pirState == HIGH ? "Presence detectee" : "Aucune presence");
    Serial.print("LDR : ");
    Serial.print(ldrValue);
    Serial.print(" (");
    Serial.print(luminositePct);
    Serial.println(" %)");
    Serial.print("Luminosite : ");
    Serial.println(obscurite ? "Obscurite" : "Lumiere suffisante");
    Serial.print("Temporisation : ");
    Serial.println(detectionActive ? "Active" : "Ecoulee");
    Serial.print("Mode : ");
    Serial.println(modeAuto ? "Automatique" : "Manuel");
    Serial.print("Heure simulee : ");
    Serial.print(heureSimulee());
    Serial.print("h | ");
    Serial.println(estModeNuit() ? "MODE NUIT (intensite reduite)" : "Journee");
  }
}

// =========================================================
// Heure simulée avec millis() (pas de RTC dans ce montage)
int heureSimulee() {
  return (heureDepart + (millis() / dureeHeureSimulee)) % 24;
}

// Vrai si on est en période de nuit (22h -> 6h)
bool estModeNuit() {
  int h = heureSimulee();
  return (h >= 22 || h < 6);
}

// =========================================================
// Applique un changement de mode et gère le chrono du mode manuel
void appliquerMode(bool nouveauAuto) {
  modeAuto = nouveauAuto;
  if (!modeAuto) {
    debutModeManuel = millis();
  }
  alerteManuelEnvoyee = false;
}

// Commande la LED principale et cumule le temps d'allumage
void commanderLed(int valeur) {
  ledcWrite(LED_MAIN_PIN, valeur);
  intensiteActuelle = valeur;
  bool maintenantAllumee = (valeur > 0);
  if (maintenantAllumee && !ledEstAllumee) {
    instantAllumage = millis();
    ledEstAllumee = true;
  } else if (!maintenantAllumee && ledEstAllumee) {
    dureeEclairageMs += millis() - instantAllumage;
    ledEstAllumee = false;
  }
}

// Estimation progressive de la consommation (intégration dans le temps)
void calculerConsommation() {
  unsigned long maintenant = millis();
  float dtHeures = (maintenant - dernierCalculConso) / 3600000.0;
  dernierCalculConso = maintenant;

  float puissance = LED_PUISSANCE_W * (intensiteActuelle / 255.0);  // W
  energieWh += puissance * dtHeures;                                // Wh

  Blynk.virtualWrite(V5, energieWh);   // envoi vers Blynk

  Serial.print("Consommation estimee : ");
  Serial.print(energieWh, 4);
  Serial.println(" Wh");
}

// Envoi des données vers Blynk (toutes les secondes)
void envoyerVersBlynk() {
  Blynk.virtualWrite(V0, luminositePct);
  Blynk.virtualWrite(V1, pirState);

  unsigned long total = dureeEclairageMs;
  if (ledEstAllumee) total += millis() - instantAllumage;
  Blynk.virtualWrite(V4, (int)(total / 1000));  // secondes cumulées
}

// Démarre un bip sans bloquer le programme
void demarrerBip(unsigned int frequence) {
  tone(BUZZER_PIN, frequence);
  buzzerDebut = millis();
  buzzerActif = true;
}

// Coupe le bip quand la durée est écoulée
void gererBuzzer() {
  if (buzzerActif && millis() - buzzerDebut >= buzzerDuree) {
    noTone(BUZZER_PIN);
    buzzerActif = false;
  }
}

// Tests matériels au démarrage (LED, buzzer)
void testMateriel() {
  Serial.println("Test LED principale PWM...");
  for (int i = 0; i <= 255; i += 15) { ledcWrite(LED_MAIN_PIN, i); delay(50); }
  for (int i = 255; i >= 0; i -= 15) { ledcWrite(LED_MAIN_PIN, i); delay(50); }
  ledcWrite(LED_MAIN_PIN, 0);

  Serial.println("Test LED verte...");
  digitalWrite(LED_GREEN_PIN, HIGH);
  delay(500);
  digitalWrite(LED_GREEN_PIN, LOW);

  Serial.println("Test buzzer...");
  tone(BUZZER_PIN, 1000);
  delay(300);
  noTone(BUZZER_PIN);
}
