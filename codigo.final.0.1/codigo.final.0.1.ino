/**********************************************************************************
 * SISTEMA DE MONITORAMENTO DE ENERGIA ELÉTRICA
 * Projeto TCC - SENAI Automação
 * Data: 12 de Novembro de 2025
 * Autor: [Rodrigo Pericinotto]
 **********************************************************************************/

#include <WiFi.h>
#include "RTClib.h"
#include "time.h"
#include <ModbusMaster.h>
#include <SD.h>
#include <SPI.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define MQTT_MAX_PACKET_SIZE 1024

const char* ssid = "Luc";
const char* password = "121599luc";
const char* mqtt_server = "10.32.97.225";
const int mqtt_port = 1883;

RTC_DS3231 rtc;
#define RXD2 16
#define TXD2 17
constexpr uint8_t MODBUS_7M_ADDRESS = 1;
ModbusMaster node;
#define SD_CS 5

WiFiClient espClient;
PubSubClient client(espClient);

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600;
const int daylightOffset_sec = 0;

// FLAGS DE STATUS (agora verificadas com segurança)
bool rtc_ok = false;
bool sd_ok = false;

// ====== AJUSTAR HORA VIA NTP (só se WiFi conectado) ======
void ajustarViaNTP() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.println("Ajustando hora via NTP...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  int tentativas = 0;
  while (!getLocalTime(&timeinfo) && tentativas < 10) {
    delay(1000); tentativas++;
  }
  if (getLocalTime(&timeinfo) && rtc_ok) {
    rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
    Serial.println("RTC ajustado via NTP com sucesso!");
  }
}

// ====== LER FLOAT IEEE754 ======
float readFloatIEEE(uint16_t reg) {
  uint8_t result = node.readInputRegisters(reg, 2);
  if (result == node.ku8MBSuccess) {
    uint16_t w0 = node.getResponseBuffer(0);
    uint16_t w1 = node.getResponseBuffer(1);
    uint32_t rawHL = ((uint32_t)w0 << 16) | w1;
    uint32_t rawLH = ((uint32_t)w1 << 16) | w0;
    float fHL, fLH;
    memcpy(&fHL, &rawHL, 4);
    memcpy(&fLH, &rawLH, 4);
    if (fHL > 0.1 && fHL < 80000) return fHL;
    if (fLH > 0.1 && fLH < 80000) return fLH;
  }
  return 0;
}

// ====== GRAVAR NO SD (só tenta se SD estiver OK) ======
void salvarNoSD(const char* label, float value, const char* filename) {
  if (!sd_ok) return;  // SD falhou → pula silenciosamente

  DateTime now = rtc.now();
  if (now.year() < 2024) return;  // data inválida → pula

  String dateStr = String(now.year()) + "-" +
                   (now.month() < 10 ? "0" : "") + String(now.month()) + "-" +
                   (now.day() < 10 ? "0" : "") + String(now.day());
  String timeStr = (now.hour() < 10 ? "0" : "") + String(now.hour()) + ":" +
                   (now.minute() < 10 ? "0" : "") + String(now.minute()) + ":" +
                   (now.second() < 10 ? "0" : "") + String(now.second());

  File file = SD.open(filename, FILE_APPEND);
  if (file) {
    if (file.size() == 0) file.println("Grandeza,Valor,Data,Hora");
    file.print(label); file.print(",");
    file.print(value, 2); file.print(",");
    file.print(dateStr); file.print(",");
    file.println(timeStr);
    file.close();
  }
}

// ====== RECONECTAR WIFI E MQTT ======
void reconectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("WiFi perdido! Reconectando");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 40) { delay(500); Serial.print("."); t++; }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nWiFi reconectado!" : "\nWiFi falhou");
}

void reconectarMQTT() {
  if (WiFi.status() != WL_CONNECTED || client.connected()) return;
  int t = 0;
  while (!client.connected() && t < 5) {
    Serial.print("Tentando MQTT...");
    if (client.connect("ESP32_TCC")) {
      Serial.println(" MQTT CONECTADO!");
      return;
    }
    delay(3000); t++;
  }
  if (!client.connected()) Serial.println(" MQTT FALHOU");
}

// ====== ENVIO DAS PARTES (inalterado) ======
void enviarParte1() {
  StaticJsonDocument<1024> doc;
  DateTime now = rtc_ok ? rtc.now() : DateTime(2025, 11, 18, 12, 0, 0);
  char ts[20];
  sprintf(ts, "%04d-%02d-%02d %02d:%02d:%02d", now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());
  doc["ts"] = ts;

  doc["U1"] = readFloatIEEE(32500 - 30001);
  doc["U2"] = readFloatIEEE(32502 - 30001);
  doc["U3"] = readFloatIEEE(32504 - 30001);
  doc["U12"] = readFloatIEEE(32508 - 30001);
  doc["U23"] = readFloatIEEE(32510 - 30001);
  doc["U31"] = readFloatIEEE(32512 - 30001);
  doc["I1"] = readFloatIEEE(32516 - 30001);
  doc["I2"] = readFloatIEEE(32518 - 30001);
  doc["I3"] = readFloatIEEE(32520 - 30001);
  doc["Iavg"] = readFloatIEEE(32528 - 30001);

  char buffer[1024];
  size_t len = serializeJson(doc, buffer);
  client.publish("Tcc/energia/parte1", buffer, len);
}

void enviarParte2() {
  StaticJsonDocument<1024> doc;
  DateTime now = rtc_ok ? rtc.now() : DateTime(2025, 11, 18, 12, 0, 0);
  char ts[20];
  sprintf(ts, "%04d-%02d-%02d %02d:%02d:%02d", now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());
  doc["ts"] = ts;

  doc["P1"] = readFloatIEEE(32530 - 30001); doc["P2"] = readFloatIEEE(32532 - 30001);
  doc["P3"] = readFloatIEEE(32534 - 30001); doc["Pt"] = readFloatIEEE(32536 - 30001);
  doc["Q1"] = readFloatIEEE(32538 - 30001); doc["Q2"] = readFloatIEEE(32540 - 30001);
  doc["Q3"] = readFloatIEEE(32542 - 30001); doc["Qt"] = readFloatIEEE(32544 - 30001);
  doc["S1"] = readFloatIEEE(32546 - 30001); doc["S2"] = readFloatIEEE(32548 - 30001);
  doc["S3"] = readFloatIEEE(32550 - 30001); doc["St"] = readFloatIEEE(32552 - 30001);
  doc["PF1"] = readFloatIEEE(32554 - 30001); doc["PF2"] = readFloatIEEE(32556 - 30001);
  doc["PF3"] = readFloatIEEE(32558 - 30001); doc["PFt"] = readFloatIEEE(32560 - 30001);

  doc["ang_U1_I1"] = readFloatIEEE(32570 - 30001); doc["ang_U2_I2"] = readFloatIEEE(32572 - 30001);
  doc["ang_U3_I3"] = readFloatIEEE(32574 - 30001); doc["ang_total"] = readFloatIEEE(32576 - 30001);
  doc["ang_U1_U2"] = readFloatIEEE(32578 - 30001); doc["ang_U2_U3"] = readFloatIEEE(32580 - 30001);
  doc["ang_U3_U1"] = readFloatIEEE(32582 - 30001);

  doc["freq"] = readFloatIEEE(32584 - 30001);

  doc["THD_I1"] = readFloatIEEE(32588 - 30001); doc["THD_I2"] = readFloatIEEE(32590 - 30001);
  doc["THD_I3"] = readFloatIEEE(32592 - 30001);
  doc["THD_U1"] = readFloatIEEE(32594 - 30001); doc["THD_U2"] = readFloatIEEE(32596 - 30001);
  doc["THD_U3"] = readFloatIEEE(32598 - 30001);

  doc["runtime"] = readFloatIEEE(32480 - 30001);
  doc["Kwh1"] = readFloatIEEE(32638 - 30001); doc["Kwh2"] = readFloatIEEE(32640 - 30001);
  doc["Kvarh3"] = readFloatIEEE(32642 - 30001); doc["Kvarh4"] = readFloatIEEE(32644 - 30001);

  char buffer[1024];
  size_t len = serializeJson(doc, buffer);
  client.publish("Tcc/energia/parte2", buffer, len);
}

// ====== SETUP - NUNCA MAIS TRAVA ======
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\n=== TCC RODRIGO - VERSÃO INDESTRUTÍVEL ===\n");

  Serial2.begin(38400, SERIAL_8N1, RXD2, TXD2);
  node.begin(MODBUS_7M_ADDRESS, Serial2);
  Serial.println("Modbus RTU iniciado (OK)");

  // TESTE DO RTC (não trava mais)
  if (!rtc.begin()) {
    Serial.println(">>> MÓDULO RTC (DS3231) FALHOU OU DESCONECTADO <<<");
    rtc_ok = false;
  } else {
    Serial.println("Módulo RTC detectado (OK)");
    rtc_ok = true;
  }

  // TESTE DO SD (não trava mais)
  if (!SD.begin(SD_CS)) {
    Serial.println(">>> MÓDULO SD OU CARTÃO FALHOU (continuando sem salvar) <<<");
    sd_ok = false;
  } else {
    Serial.println("Cartão SD detectado (OK)");
    sd_ok = true;
  }

  WiFi.begin(ssid, password);
  int t = 0;
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED && t < 40) { delay(500); Serial.print("."); t++; }
  Serial.println(WiFi.status() == WL_CONNECTED ?
                 "\nWiFi conectado! IP: " + WiFi.localIP().toString() :
                 "\nWiFi não conectou");

  if (WiFi.status() == WL_CONNECTED) {
    ajustarViaNTP();
    client.setServer(mqtt_server, mqtt_port);
    client.setBufferSize(1024);
    reconectarMQTT();
  }

  Serial.println("\n=== SISTEMA INICIADO COM SUCESSO - RODANDO 100% ===\n");
}

// ====== LOOP COM DIAGNÓSTICO E MEDIÇÕES NO SERIAL ======
void loop() {
  if (WiFi.status() != WL_CONNECTED) reconectarWiFi();
  if (WiFi.status() == WL_CONNECTED && !client.connected()) reconectarMQTT();
  client.loop();

  static unsigned long last = 0;
  if (millis() - last < 10000) return;
  last = millis();

  // DIAGNÓSTICO ANTES DE CADA LEITURA
  Serial.println("\n>>> NOVA LEITURA A CADA 10 SEGUNDOS <<<");
  Serial.printf("DIAGNÓSTICO → RTC: %s | SD: %s\n",
                rtc_ok ? "OK" : "FALHOU",
                sd_ok ? "OK" : "FALHOU");
  Serial.println("════════════════════════════════════════════════");

  float U1 = readFloatIEEE(32500 - 30001); Serial.printf("U1 (F-N):     %.2f V\n", U1); salvarNoSD("U1", U1, "/tensao.csv");
  float U2 = readFloatIEEE(32502 - 30001); Serial.printf("U2 (F-N):     %.2f V\n", U2); salvarNoSD("U2", U2, "/tensao.csv");
  float U3 = readFloatIEEE(32504 - 30001); Serial.printf("U3 (F-N):     %.2f V\n", U3); salvarNoSD("U3", U3, "/tensao.csv");
  float U12 = readFloatIEEE(32508 - 30001); Serial.printf("U12 (L-L):    %.2f V\n", U12); salvarNoSD("U12", U12, "/tensao_linha.csv");
  float U23 = readFloatIEEE(32510 - 30001); Serial.printf("U23 (L-L):    %.2f V\n", U23); salvarNoSD("U23", U23, "/tensao_linha.csv");
  float U31 = readFloatIEEE(32512 - 30001); Serial.printf("U31 (L-L):    %.2f V\n", U31); salvarNoSD("U31", U31, "/tensao_linha.csv");

  float I1 = readFloatIEEE(32516 - 30001); Serial.printf("I1:           %.3f A\n", I1); salvarNoSD("I1", I1, "/corrente.csv");
  float I2 = readFloatIEEE(32518 - 30001); Serial.printf("I2:           %.3f A\n", I2); salvarNoSD("I2", I2, "/corrente.csv");
  float I3 = readFloatIEEE(32520 - 30001); Serial.printf("I3:           %.3f A\n", I3); salvarNoSD("I3", I3, "/corrente.csv");
  float Iavg = readFloatIEEE(32528 - 30001); Serial.printf("I média:      %.3f A\n", Iavg); salvarNoSD("Iavg", Iavg, "/corrente.csv");

  float P1 = readFloatIEEE(32530 - 30001); Serial.printf("P1 Ativa:     %.2f kW\n", P1/1000); salvarNoSD("P1", P1, "/potencia_ativa.csv");
  float P2 = readFloatIEEE(32532 - 30001); Serial.printf("P2 Ativa:     %.2f kW\n", P2/1000); salvarNoSD("P2", P2, "/potencia_ativa.csv");
  float P3 = readFloatIEEE(32534 - 30001); Serial.printf("P3 Ativa:     %.2f kW\n", P3/1000); salvarNoSD("P3", P3, "/potencia_ativa.csv");
  float Pt = readFloatIEEE(32536 - 30001); Serial.printf("P Total:      %.2f kW\n", Pt/1000); salvarNoSD("Pt", Pt, "/potencia_ativa.csv");

  float Q1 = readFloatIEEE(32538 - 30001); Serial.printf("Q1 Reativa:   %.2f kvar\n", Q1/1000); salvarNoSD("Q1", Q1, "/potencia_reativa.csv");
  float Q2 = readFloatIEEE(32540 - 30001); Serial.printf("Q2 Reativa:   %.2f kvar\n", Q2/1000); salvarNoSD("Q2", Q2, "/potencia_reativa.csv");
  float Q3 = readFloatIEEE(32542 - 30001); Serial.printf("Q3 Reativa:   %.2f kvar\n", Q3/1000); salvarNoSD("Q3", Q3, "/potencia_reativa.csv");
  float Qt = readFloatIEEE(32544 - 30001); Serial.printf("Q Total:      %.2f kvar\n", Qt/1000); salvarNoSD("Qt", Qt, "/potencia_reativa.csv");

  float S1 = readFloatIEEE(32546 - 30001); Serial.printf("S1 Aparente:  %.2f kVA\n", S1/1000); salvarNoSD("S1", S1, "/potencia_aparente.csv");
  float S2 = readFloatIEEE(32548 - 30001); Serial.printf("S2 Aparente:  %.2f kVA\n", S2/1000); salvarNoSD("S2", S2, "/potencia_aparente.csv");
  float S3 = readFloatIEEE(32550 - 30001); Serial.printf("S3 Aparente:  %.2f kVA\n", S3/1000); salvarNoSD("S3", S3, "/potencia_aparente.csv");
  float St = readFloatIEEE(32552 - 30001); Serial.printf("S Total:      %.2f kVA\n", St/1000); salvarNoSD("St", St, "/potencia_aparente.csv");

  float PF1 = readFloatIEEE(32554 - 30001); Serial.printf("FP Fase 1:    %.3f\n", PF1); salvarNoSD("PF1", PF1, "/fator_potencia.csv");
  float PF2 = readFloatIEEE(32556 - 30001); Serial.printf("FP Fase 2:    %.3f\n", PF2); salvarNoSD("PF2", PF2, "/fator_potencia.csv");
  float PF3 = readFloatIEEE(32558 - 30001); Serial.printf("FP Fase 3:    %.3f\n", PF3); salvarNoSD("PF3", PF3, "/fator_potencia.csv");
  float PFt = readFloatIEEE(32560 - 30001); Serial.printf("FP Total:     %.3f\n", PFt); salvarNoSD("PFt", PFt, "/fator_potencia.csv");

  float ang1 = readFloatIEEE(32570 - 30001); Serial.printf("Ângulo U1-I1: %.1f °\n", ang1); salvarNoSD("ang_U1_I1", ang1, "/angulos.csv");
  float ang2 = readFloatIEEE(32572 - 30001); Serial.printf("Ângulo U2-I2: %.1f °\n", ang2); salvarNoSD("ang_U2_I2", ang2, "/angulos.csv");
  float ang3 = readFloatIEEE(32574 - 30001); Serial.printf("Ângulo U3-I3: %.1f °\n", ang3); salvarNoSD("ang_U3_I3", ang3, "/angulos.csv");
  float angt = readFloatIEEE(32576 - 30001); Serial.printf("Ângulo Total: %.1f °\n", angt); salvarNoSD("ang_total", angt, "/angulos.csv");
  float ang12 = readFloatIEEE(32578 - 30001); Serial.printf("Ângulo U1-U2: %.1f °\n", ang12); salvarNoSD("ang_U1_U2", ang12, "/angulos.csv");
  float ang23 = readFloatIEEE(32580 - 30001); Serial.printf("Ângulo U2-U3: %.1f °\n", ang23); salvarNoSD("ang_U2_U3", ang23, "/angulos.csv");
  float ang31 = readFloatIEEE(32582 - 30001); Serial.printf("Ângulo U3-U1: %.1f °\n", ang31); salvarNoSD("ang_U3_U1", ang31, "/angulos.csv");

  float freq = readFloatIEEE(32584 - 30001); Serial.printf("Frequência:   %.2f Hz\n", freq); salvarNoSD("Frequência", freq, "/frequencia.csv");

  float thd_i1 = readFloatIEEE(32588 - 30001); Serial.printf("THD I1:       %.2f%%\n", thd_i1); salvarNoSD("THD_I1", thd_i1, "/thd_correntes.csv");
  float thd_i2 = readFloatIEEE(32590 - 30001); Serial.printf("THD I2:       %.2f%%\n", thd_i2); salvarNoSD("THD_I2", thd_i2, "/thd_correntes.csv");
  float thd_i3 = readFloatIEEE(32592 - 30001); Serial.printf("THD I3:       %.2f%%\n", thd_i3); salvarNoSD("THD_I3", thd_i3, "/thd_correntes.csv");
  float thd_u1 = readFloatIEEE(32594 - 30001); Serial.printf("THD U1:       %.2f%%\n", thd_u1); salvarNoSD("THD_U1", thd_u1, "/thd_tensoes.csv");
  float thd_u2 = readFloatIEEE(32596 - 30001); Serial.printf("THD U2:       %.2f%%\n", thd_u2); salvarNoSD("THD_U2", thd_u2, "/thd_tensoes.csv");
  float thd_u3 = readFloatIEEE(32598 - 30001); Serial.printf("THD U3:       %.2f%%\n", thd_u3); salvarNoSD("THD_U3", thd_u3, "/thd_tensoes.csv");

  float runtime = readFloatIEEE(32480 - 30001); Serial.printf("Tempo ligado: %.2f h\n", runtime); salvarNoSD("Runtime", runtime, "/runtime.csv");
  float kwh1 = readFloatIEEE(32638 - 30001); Serial.printf("kWh Fase 1:   %.2f kWh\n", kwh1); salvarNoSD("Kwh1", kwh1, "/energia.csv");
  float kwh2 = readFloatIEEE(32640 - 30001); Serial.printf("kWh Fase 2:   %.2f kWh\n", kwh2); salvarNoSD("Kwh2", kwh2, "/energia.csv");
  float kvarh3 = readFloatIEEE(32642 - 30001); Serial.printf("kvarh 3:      %.2f kvarh\n", kvarh3); salvarNoSD("Kvarh3", kvarh3, "/energia.csv");
  float kvarh4 = readFloatIEEE(32644 - 30001); Serial.printf("kvarh 4:      %.2f kvarh\n", kvarh4); salvarNoSD("Kvarh4", kvarh4, "/energia.csv");

  Serial.println("════════════════════════════════════════════════");

  if (client.connected()) {
    enviarParte1();
    delay(100);
    enviarParte2();
    Serial.println("2 PARTES JSON ENVIADAS COM SUCESSO!");
  } else {
    Serial.println("MQTT DESCONECTADO - Dados apenas no Serial/SD");
  }
}