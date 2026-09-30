/**********************************************************************************
 * SISTEMA DE MONITORAMENTO DE ENERGIA ELÉTRICA
 * Projeto TCC - SENAI Automação
 * Data: 12 de Novembro de 2025
 * Autor: [Rodrigo Pericinotto]
 **********************************************************************************/

// ============================ BIBLIOTECAS ============================
#include <WiFi.h>            // Biblioteca do ESP32 para conexão Wi-Fi
#include "RTClib.h"          // Biblioteca para o relógio de tempo real (RTC DS3231)
#include "time.h"            // Funções de data/hora do ESP32 (usadas para buscar a hora via NTP)
#include <ModbusMaster.h>    // Biblioteca para o ESP32 atuar como "mestre" Modbus RTU (conversa com o Finder 7M38)
#include <SD.h>              // Biblioteca para ler/gravar no cartão MicroSD
#include <SPI.h>             // Protocolo SPI, usado na comunicação com o módulo MicroSD
#include <PubSubClient.h>    // Cliente MQTT (envia os dados ao Node-RED)
#include <ArduinoJson.h>     // Biblioteca para montar os pacotes de dados no formato JSON

// Define o tamanho máximo de pacote MQTT (o tamanho efetivo também é configurado depois, com setBufferSize)
#define MQTT_MAX_PACKET_SIZE 1024

// ============================ CONFIGURAÇÕES DE REDE ============================
const char* ssid = "Luc";                // Nome da rede Wi-Fi à qual o ESP32 vai se conectar
const char* password = "121599luc";      // Senha da rede Wi-Fi
const char* mqtt_server = "10.32.97.225"; // Endereço IP do broker MQTT (máquina onde roda o Node-RED/Mosquitto)
const int mqtt_port = 1883;              // Porta padrão do protocolo MQTT

// ============================ OBJETOS E PINOS ============================
RTC_DS3231 rtc;                          // Cria o objeto do relógio de tempo real DS3231
#define RXD2 16                          // Pino GPIO16 do ESP32 = RX da Serial2 (recebe dados do MAX3485)
#define TXD2 17                          // Pino GPIO17 do ESP32 = TX da Serial2 (envia dados ao MAX3485)
constexpr uint8_t MODBUS_7M_ADDRESS = 1; // Endereço Modbus (escravo) do medidor Finder 7M38 na rede RS-485
ModbusMaster node;                       // Cria o objeto mestre Modbus que fará as leituras dos registradores
#define SD_CS 5                          // Pino GPIO5 usado como Chip Select (CS) do módulo MicroSD

WiFiClient espClient;                    // Cliente de rede TCP usado como base para a conexão MQTT
PubSubClient client(espClient);          // Cliente MQTT que utiliza o espClient para se comunicar com o broker

// ============================ CONFIGURAÇÃO DO NTP (HORA NA INTERNET) ============================
const char* ntpServer = "pool.ntp.org";  // Servidor de horário na internet (NTP)
const long gmtOffset_sec = -3 * 3600;    // Fuso horário de Brasília: GMT-3, convertido para segundos
const int daylightOffset_sec = 0;        // Sem horário de verão (0 segundos de ajuste)

// FLAGS DE STATUS (agora verificadas com segurança)
bool rtc_ok = false;                     // Indica se o módulo RTC foi detectado (true) ou não (false)
bool sd_ok = false;                      // Indica se o cartão SD foi detectado (true) ou não (false)

// ====== AJUSTAR HORA VIA NTP (só se WiFi conectado) ======
// Busca a hora certa na internet e acerta o relógio RTC com ela
void ajustarViaNTP() {
  if (WiFi.status() != WL_CONNECTED) return;      // Sem Wi-Fi não há como consultar o NTP, então sai da função
  Serial.println("Ajustando hora via NTP...");    // Mensagem de acompanhamento no monitor serial
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); // Configura o fuso e o servidor NTP e inicia a sincronização
  struct tm timeinfo;                             // Estrutura que guarda data e hora (ano, mês, dia, hora...)
  int tentativas = 0;                             // Contador de tentativas de obter a hora
  while (!getLocalTime(&timeinfo) && tentativas < 10) { // Tenta obter a hora até 10 vezes
    delay(1000); tentativas++;                    // Espera 1 segundo entre as tentativas e incrementa o contador
  }
  if (getLocalTime(&timeinfo) && rtc_ok) {        // Se conseguiu a hora da internet E o RTC está funcionando...
    rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, // ...grava no RTC: ano (tm_year conta desde 1900), mês (tm_mon começa em 0), dia
                        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));           // ...hora, minuto e segundo
    Serial.println("RTC ajustado via NTP com sucesso!"); // Confirma o ajuste no monitor serial
  }
}

// ====== LER FLOAT IEEE754 ======
// Lê 2 registradores Modbus (32 bits) e converte para um número decimal (float).
// 'reg' é o endereço do registrador inicial dentro do medidor.
float readFloatIEEE(uint16_t reg) {
  uint8_t result = node.readInputRegisters(reg, 2);   // Pede ao medidor a leitura de 2 registradores de entrada a partir de 'reg'
  if (result == node.ku8MBSuccess) {                  // Se a comunicação Modbus deu certo...
    uint16_t w0 = node.getResponseBuffer(0);          // Primeira palavra (16 bits) recebida
    uint16_t w1 = node.getResponseBuffer(1);          // Segunda palavra (16 bits) recebida
    uint32_t rawHL = ((uint32_t)w0 << 16) | w1;       // Junta as palavras na ordem "alta primeiro" (w0 = parte alta, w1 = parte baixa)
    uint32_t rawLH = ((uint32_t)w1 << 16) | w0;       // Junta as palavras na ordem inversa (w1 = parte alta, w0 = parte baixa)
    float fHL, fLH;                                   // Variáveis que receberão o valor decimal de cada ordem
    memcpy(&fHL, &rawHL, 4);                          // Reinterpreta os 32 bits (ordem HL) como float IEEE754
    memcpy(&fLH, &rawLH, 4);                          // Reinterpreta os 32 bits (ordem LH) como float IEEE754
    if (fHL > 0.1 && fHL < 80000) return fHL;         // Se o valor na ordem HL está numa faixa plausível, retorna ele
    if (fLH > 0.1 && fLH < 80000) return fLH;         // Senão, testa a ordem LH e retorna se estiver na faixa plausível
  }
  return 0;                                           // Falha de comunicação ou valor fora da faixa (≤ 0.1 ou ≥ 80000) retorna 0
}

// ====== GRAVAR NO SD (só tenta se SD estiver OK) ======
// Salva uma medição (nome, valor, data e hora) em uma linha de um arquivo CSV no cartão
void salvarNoSD(const char* label, float value, const char* filename) {
  if (!sd_ok) return;  // SD falhou → pula silenciosamente

  DateTime now = rtc.now();       // Lê a data e hora atuais do RTC
  if (now.year() < 2024) return;  // data inválida → pula

  // Monta a data no formato AAAA-MM-DD, colocando zero à esquerda em mês e dia menores que 10
  String dateStr = String(now.year()) + "-" +
                   (now.month() < 10 ? "0" : "") + String(now.month()) + "-" +
                   (now.day() < 10 ? "0" : "") + String(now.day());
  // Monta a hora no formato HH:MM:SS, colocando zero à esquerda quando necessário
  String timeStr = (now.hour() < 10 ? "0" : "") + String(now.hour()) + ":" +
                   (now.minute() < 10 ? "0" : "") + String(now.minute()) + ":" +
                   (now.second() < 10 ? "0" : "") + String(now.second());

  File file = SD.open(filename, FILE_APPEND);  // Abre o arquivo em modo "acrescentar" (adiciona ao final, sem apagar o que já existe)
  if (file) {                                  // Se o arquivo abriu corretamente...
    if (file.size() == 0) file.println("Grandeza,Valor,Data,Hora"); // Se o arquivo é novo (vazio), escreve primeiro o cabeçalho das colunas
    file.print(label); file.print(",");        // Escreve o nome da grandeza (ex.: U1) seguido de vírgula (separador do CSV)
    file.print(value, 2); file.print(",");     // Escreve o valor com 2 casas decimais seguido de vírgula
    file.print(dateStr); file.print(",");      // Escreve a data seguida de vírgula
    file.println(timeStr);                     // Escreve a hora e pula para a próxima linha
    file.close();                              // Fecha o arquivo, garantindo que os dados sejam gravados no cartão
  }
}

// ====== RECONECTAR WIFI E MQTT ======
// Tenta restabelecer o Wi-Fi caso a conexão tenha caído
void reconectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;   // Se já está conectado, não precisa fazer nada
  Serial.print("WiFi perdido! Reconectando");  // Avisa no monitor serial que a conexão caiu
  WiFi.disconnect();                           // Encerra qualquer tentativa/estado anterior de conexão
  WiFi.begin(ssid, password);                  // Inicia nova tentativa de conexão com a rede
  int t = 0;                                   // Contador de tentativas
  while (WiFi.status() != WL_CONNECTED && t < 40) { delay(500); Serial.print("."); t++; } // Espera até 40 x 0,5 s = 20 s, imprimindo pontinhos de progresso
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nWiFi reconectado!" : "\nWiFi falhou"); // Informa se conseguiu ou não reconectar
}

// Tenta restabelecer a conexão com o broker MQTT
void reconectarMQTT() {
  if (WiFi.status() != WL_CONNECTED || client.connected()) return; // Sai se não há Wi-Fi ou se o MQTT já está conectado
  int t = 0;                                   // Contador de tentativas
  while (!client.connected() && t < 5) {       // Tenta até 5 vezes enquanto não estiver conectado
    Serial.print("Tentando MQTT...");          // Mensagem de acompanhamento
    if (client.connect("ESP32_TCC")) {         // Tenta conectar ao broker usando "ESP32_TCC" como identificação do cliente
      Serial.println(" MQTT CONECTADO!");      // Sucesso
      return;                                  // Sai da função, pois já conectou
    }
    delay(3000); t++;                          // Falhou: espera 3 segundos e tenta novamente
  }
  if (!client.connected()) Serial.println(" MQTT FALHOU"); // Após as 5 tentativas, avisa que não foi possível conectar
}

// ====== ENVIO DAS PARTES (inalterado) ======
// O envio é dividido em duas mensagens JSON (parte 1 e parte 2) para não estourar o tamanho máximo do pacote MQTT.

// PARTE 1: tensões e correntes
void enviarParte1() {
  StaticJsonDocument<1024> doc;                // Cria o documento JSON na memória, com capacidade de 1024 bytes
  DateTime now = rtc_ok ? rtc.now() : DateTime(2025, 11, 18, 12, 0, 0); // Usa a hora do RTC; se o RTC falhou, usa uma data fixa de reserva
  char ts[20];                                 // Texto que guardará o carimbo de data/hora (timestamp)
  sprintf(ts, "%04d-%02d-%02d %02d:%02d:%02d", now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second()); // Formata o timestamp como "AAAA-MM-DD HH:MM:SS"
  doc["ts"] = ts;                              // Insere o timestamp no JSON

  // Os registradores do medidor aparecem como 3xxxx na documentação; subtrair 30001 converte para o endereço usado na leitura Modbus
  doc["U1"] = readFloatIEEE(32500 - 30001);    // Tensão fase-neutro da fase 1 (V)
  doc["U2"] = readFloatIEEE(32502 - 30001);    // Tensão fase-neutro da fase 2 (V)
  doc["U3"] = readFloatIEEE(32504 - 30001);    // Tensão fase-neutro da fase 3 (V)
  doc["U12"] = readFloatIEEE(32508 - 30001);   // Tensão de linha entre fases 1 e 2 (V)
  doc["U23"] = readFloatIEEE(32510 - 30001);   // Tensão de linha entre fases 2 e 3 (V)
  doc["U31"] = readFloatIEEE(32512 - 30001);   // Tensão de linha entre fases 3 e 1 (V)
  doc["I1"] = readFloatIEEE(32516 - 30001);    // Corrente da fase 1 (A)
  doc["I2"] = readFloatIEEE(32518 - 30001);    // Corrente da fase 2 (A)
  doc["I3"] = readFloatIEEE(32520 - 30001);    // Corrente da fase 3 (A)
  doc["Iavg"] = readFloatIEEE(32528 - 30001);  // Corrente média das três fases (A)

  char buffer[1024];                           // Área de memória onde o JSON será convertido para texto
  size_t len = serializeJson(doc, buffer);     // Converte o documento JSON em texto e guarda o tamanho (len)
  client.publish("Tcc/energia/parte1", buffer, len); // Publica o JSON no tópico MQTT "Tcc/energia/parte1" (o Node-RED assina esse tópico)
}

// PARTE 2: potências, fator de potência, ângulos, frequência, THD e energia
void enviarParte2() {
  StaticJsonDocument<1024> doc;                // Cria o documento JSON na memória
  DateTime now = rtc_ok ? rtc.now() : DateTime(2025, 11, 18, 12, 0, 0); // Hora do RTC ou data fixa de reserva
  char ts[20];                                 // Texto do timestamp
  sprintf(ts, "%04d-%02d-%02d %02d:%02d:%02d", now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second()); // Formata como "AAAA-MM-DD HH:MM:SS"
  doc["ts"] = ts;                              // Insere o timestamp no JSON

  // Potências ativas (W): por fase (P1, P2, P3) e total (Pt)
  doc["P1"] = readFloatIEEE(32530 - 30001); doc["P2"] = readFloatIEEE(32532 - 30001);
  doc["P3"] = readFloatIEEE(32534 - 30001); doc["Pt"] = readFloatIEEE(32536 - 30001);
  // Potências reativas (var): por fase (Q1, Q2, Q3) e total (Qt)
  doc["Q1"] = readFloatIEEE(32538 - 30001); doc["Q2"] = readFloatIEEE(32540 - 30001);
  doc["Q3"] = readFloatIEEE(32542 - 30001); doc["Qt"] = readFloatIEEE(32544 - 30001);
  // Potências aparentes (VA): por fase (S1, S2, S3) e total (St)
  doc["S1"] = readFloatIEEE(32546 - 30001); doc["S2"] = readFloatIEEE(32548 - 30001);
  doc["S3"] = readFloatIEEE(32550 - 30001); doc["St"] = readFloatIEEE(32552 - 30001);
  // Fator de potência: por fase (PF1, PF2, PF3) e total (PFt)
  doc["PF1"] = readFloatIEEE(32554 - 30001); doc["PF2"] = readFloatIEEE(32556 - 30001);
  doc["PF3"] = readFloatIEEE(32558 - 30001); doc["PFt"] = readFloatIEEE(32560 - 30001);

  // Ângulos de defasagem entre tensão e corrente de cada fase, e o total
  doc["ang_U1_I1"] = readFloatIEEE(32570 - 30001); doc["ang_U2_I2"] = readFloatIEEE(32572 - 30001);
  doc["ang_U3_I3"] = readFloatIEEE(32574 - 30001); doc["ang_total"] = readFloatIEEE(32576 - 30001);
  // Ângulos entre as tensões das fases (U1-U2, U2-U3, U3-U1)
  doc["ang_U1_U2"] = readFloatIEEE(32578 - 30001); doc["ang_U2_U3"] = readFloatIEEE(32580 - 30001);
  doc["ang_U3_U1"] = readFloatIEEE(32582 - 30001);

  doc["freq"] = readFloatIEEE(32584 - 30001);  // Frequência da rede (Hz)

  // THD (distorção harmônica total) das correntes, em %
  doc["THD_I1"] = readFloatIEEE(32588 - 30001); doc["THD_I2"] = readFloatIEEE(32590 - 30001);
  doc["THD_I3"] = readFloatIEEE(32592 - 30001);
  // THD (distorção harmônica total) das tensões, em %
  doc["THD_U1"] = readFloatIEEE(32594 - 30001); doc["THD_U2"] = readFloatIEEE(32596 - 30001);
  doc["THD_U3"] = readFloatIEEE(32598 - 30001);

  doc["runtime"] = readFloatIEEE(32480 - 30001); // Tempo de funcionamento do medidor (horas)
  // Contadores de energia acumulada: energia ativa (kWh) e energia reativa (kvarh)
  doc["Kwh1"] = readFloatIEEE(32638 - 30001); doc["Kwh2"] = readFloatIEEE(32640 - 30001);
  doc["Kvarh3"] = readFloatIEEE(32642 - 30001); doc["Kvarh4"] = readFloatIEEE(32644 - 30001);

  char buffer[1024];                           // Área de memória para o JSON em formato de texto
  size_t len = serializeJson(doc, buffer);     // Converte o JSON em texto e guarda o tamanho
  client.publish("Tcc/energia/parte2", buffer, len); // Publica no tópico MQTT "Tcc/energia/parte2"
}

// ====== SETUP ======
// Executa uma única vez quando o ESP32 é ligado ou reiniciado: inicializa todos os módulos
void setup() {
  Serial.begin(115200);                        // Inicia o monitor serial (USB) a 115200 bits/s para mensagens de diagnóstico
  delay(2000);                                 // Aguarda 2 segundos para o monitor serial estabilizar
  Serial.println("\n\n=== TCC Sistema De Monitoramento ===\n"); // Mensagem de abertura

  Serial2.begin(38400, SERIAL_8N1, RXD2, TXD2); // Inicia a Serial2 a 38400 bps, 8 bits de dados, sem paridade, 1 stop bit, nos pinos RX=16 e TX=17 (ligados ao MAX3485)
  node.begin(MODBUS_7M_ADDRESS, Serial2);      // Inicia o Modbus: fala com o escravo de endereço 1 (Finder 7M38) pela Serial2
  Serial.println("Modbus RTU iniciado (OK)");  // Confirma a inicialização do Modbus

  // TESTE DO RTC
  if (!rtc.begin()) {                          // Tenta iniciar o RTC; retorna falso se não o encontrar
    Serial.println(">>> MÓDULO RTC (DS3231) FALHOU OU DESCONECTADO <<<"); // Avisa a falha
    rtc_ok = false;                            // Marca o RTC como indisponível
  } else {
    Serial.println("Módulo RTC detectado (OK)"); // Avisa que o RTC foi encontrado
    rtc_ok = true;                             // Marca o RTC como disponível
  }

  // TESTE DO SD
  if (!SD.begin(SD_CS)) {                      // Tenta iniciar o cartão SD usando o pino CS definido
    Serial.println(">>> MÓDULO SD OU CARTÃO FALHOU (continuando sem salvar) <<<"); // Avisa a falha
    sd_ok = false;                             // Marca o SD como indisponível (o programa segue sem gravar)
  } else {
    Serial.println("Cartão SD detectado (OK)"); // Avisa que o SD foi encontrado
    sd_ok = true;                              // Marca o SD como disponível
  }

  WiFi.begin(ssid, password);                  // Inicia a conexão com a rede Wi-Fi
  int t = 0;                                   // Contador de tentativas
  Serial.print("Conectando WiFi");             // Mensagem de acompanhamento
  while (WiFi.status() != WL_CONNECTED && t < 40) { delay(500); Serial.print("."); t++; } // Aguarda até 20 s pela conexão, imprimindo pontinhos
  Serial.println(WiFi.status() == WL_CONNECTED ?
                 "\nWiFi conectado! IP: " + WiFi.localIP().toString() :
                 "\nWiFi não conectou");      // Mostra o IP recebido se conectou, ou avisa que não conectou

  if (WiFi.status() == WL_CONNECTED) {         // Só configura internet/MQTT se o Wi-Fi conectou
    ajustarViaNTP();                           // Acerta o relógio RTC com a hora da internet
    client.setServer(mqtt_server, mqtt_port);  // Informa ao cliente MQTT o endereço e a porta do broker
    client.setBufferSize(1024);                // Aumenta o buffer MQTT para 1024 bytes (o padrão é pequeno demais para os JSONs)
    reconectarMQTT();                          // Faz a primeira conexão com o broker
  }

  Serial.println("\n=== SISTEMA INICIADO COM SUCESSO ===\n"); // Fim da inicialização
}

// ====== LOOP COM DIAGNÓSTICO E MEDIÇÕES NO SERIAL ======
// Repete continuamente: mantém as conexões vivas e, a cada 10 s, lê, exibe, grava e envia os dados
void loop() {
  if (WiFi.status() != WL_CONNECTED) reconectarWiFi();                         // Se o Wi-Fi caiu, tenta reconectar
  if (WiFi.status() == WL_CONNECTED && !client.connected()) reconectarMQTT();  // Se há Wi-Fi mas o MQTT caiu, tenta reconectar
  client.loop();                               // Mantém o cliente MQTT ativo (processa mensagens e mantém a conexão)

  static unsigned long last = 0;               // Guarda o instante (em ms) da última leitura; 'static' preserva o valor entre repetições do loop
  if (millis() - last < 10000) return;         // Se ainda não passaram 10 segundos desde a última leitura, sai e volta ao início do loop
  last = millis();                             // Passaram 10 s: atualiza o instante da última leitura

  // DIAGNÓSTICO ANTES DE CADA LEITURA
  Serial.println("\n>>> NOVA LEITURA A CADA 10 SEGUNDOS <<<");  // Cabeçalho de cada ciclo de leitura
  Serial.printf("DIAGNÓSTICO → RTC: %s | SD: %s\n",
                rtc_ok ? "OK" : "FALHOU",
                sd_ok ? "OK" : "FALHOU");      // Mostra no serial se o RTC e o SD estão funcionando
  Serial.println("════════════════════════════════════════════════"); // Linha separadora visual

  // Cada linha abaixo faz 3 coisas: (1) lê o registrador do medidor, (2) mostra o valor no monitor serial, (3) salva no arquivo CSV correspondente do SD

  // --- TENSÕES FASE-NEUTRO (arquivo tensao.csv) ---
  float U1 = readFloatIEEE(32500 - 30001); Serial.printf("U1 (F-N):     %.2f V\n", U1); salvarNoSD("U1", U1, "/tensao.csv"); // Tensão da fase 1
  float U2 = readFloatIEEE(32502 - 30001); Serial.printf("U2 (F-N):     %.2f V\n", U2); salvarNoSD("U2", U2, "/tensao.csv"); // Tensão da fase 2
  float U3 = readFloatIEEE(32504 - 30001); Serial.printf("U3 (F-N):     %.2f V\n", U3); salvarNoSD("U3", U3, "/tensao.csv"); // Tensão da fase 3
  // --- TENSÕES DE LINHA (arquivo tensao_linha.csv) ---
  float U12 = readFloatIEEE(32508 - 30001); Serial.printf("U12 (L-L):    %.2f V\n", U12); salvarNoSD("U12", U12, "/tensao_linha.csv"); // Tensão entre fases 1 e 2
  float U23 = readFloatIEEE(32510 - 30001); Serial.printf("U23 (L-L):    %.2f V\n", U23); salvarNoSD("U23", U23, "/tensao_linha.csv"); // Tensão entre fases 2 e 3
  float U31 = readFloatIEEE(32512 - 30001); Serial.printf("U31 (L-L):    %.2f V\n", U31); salvarNoSD("U31", U31, "/tensao_linha.csv"); // Tensão entre fases 3 e 1

  // --- CORRENTES (arquivo corrente.csv) ---
  float I1 = readFloatIEEE(32516 - 30001); Serial.printf("I1:           %.3f A\n", I1); salvarNoSD("I1", I1, "/corrente.csv");       // Corrente da fase 1
  float I2 = readFloatIEEE(32518 - 30001); Serial.printf("I2:           %.3f A\n", I2); salvarNoSD("I2", I2, "/corrente.csv");       // Corrente da fase 2
  float I3 = readFloatIEEE(32520 - 30001); Serial.printf("I3:           %.3f A\n", I3); salvarNoSD("I3", I3, "/corrente.csv");       // Corrente da fase 3
  float Iavg = readFloatIEEE(32528 - 30001); Serial.printf("I média:      %.3f A\n", Iavg); salvarNoSD("Iavg", Iavg, "/corrente.csv"); // Corrente média

  // --- POTÊNCIA ATIVA (arquivo potencia_ativa.csv) --- valor dividido por 1000 no serial para exibir em kW; no SD o valor fica em W
  float P1 = readFloatIEEE(32530 - 30001); Serial.printf("P1 Ativa:     %.2f kW\n", P1/1000); salvarNoSD("P1", P1, "/potencia_ativa.csv"); // Potência ativa da fase 1
  float P2 = readFloatIEEE(32532 - 30001); Serial.printf("P2 Ativa:     %.2f kW\n", P2/1000); salvarNoSD("P2", P2, "/potencia_ativa.csv"); // Potência ativa da fase 2
  float P3 = readFloatIEEE(32534 - 30001); Serial.printf("P3 Ativa:     %.2f kW\n", P3/1000); salvarNoSD("P3", P3, "/potencia_ativa.csv"); // Potência ativa da fase 3
  float Pt = readFloatIEEE(32536 - 30001); Serial.printf("P Total:      %.2f kW\n", Pt/1000); salvarNoSD("Pt", Pt, "/potencia_ativa.csv"); // Potência ativa total

  // --- POTÊNCIA REATIVA (arquivo potencia_reativa.csv) --- exibida em kvar no serial
  float Q1 = readFloatIEEE(32538 - 30001); Serial.printf("Q1 Reativa:   %.2f kvar\n", Q1/1000); salvarNoSD("Q1", Q1, "/potencia_reativa.csv"); // Reativa da fase 1
  float Q2 = readFloatIEEE(32540 - 30001); Serial.printf("Q2 Reativa:   %.2f kvar\n", Q2/1000); salvarNoSD("Q2", Q2, "/potencia_reativa.csv"); // Reativa da fase 2
  float Q3 = readFloatIEEE(32542 - 30001); Serial.printf("Q3 Reativa:   %.2f kvar\n", Q3/1000); salvarNoSD("Q3", Q3, "/potencia_reativa.csv"); // Reativa da fase 3
  float Qt = readFloatIEEE(32544 - 30001); Serial.printf("Q Total:      %.2f kvar\n", Qt/1000); salvarNoSD("Qt", Qt, "/potencia_reativa.csv"); // Reativa total

  // --- POTÊNCIA APARENTE (arquivo potencia_aparente.csv) --- exibida em kVA no serial
  float S1 = readFloatIEEE(32546 - 30001); Serial.printf("S1 Aparente:  %.2f kVA\n", S1/1000); salvarNoSD("S1", S1, "/potencia_aparente.csv"); // Aparente da fase 1
  float S2 = readFloatIEEE(32548 - 30001); Serial.printf("S2 Aparente:  %.2f kVA\n", S2/1000); salvarNoSD("S2", S2, "/potencia_aparente.csv"); // Aparente da fase 2
  float S3 = readFloatIEEE(32550 - 30001); Serial.printf("S3 Aparente:  %.2f kVA\n", S3/1000); salvarNoSD("S3", S3, "/potencia_aparente.csv"); // Aparente da fase 3
  float St = readFloatIEEE(32552 - 30001); Serial.printf("S Total:      %.2f kVA\n", St/1000); salvarNoSD("St", St, "/potencia_aparente.csv"); // Aparente total

  // --- FATOR DE POTÊNCIA (arquivo fator_potencia.csv) --- exibido com 3 casas decimais
  float PF1 = readFloatIEEE(32554 - 30001); Serial.printf("FP Fase 1:    %.3f\n", PF1); salvarNoSD("PF1", PF1, "/fator_potencia.csv");  // FP da fase 1
  float PF2 = readFloatIEEE(32556 - 30001); Serial.printf("FP Fase 2:    %.3f\n", PF2); salvarNoSD("PF2", PF2, "/fator_potencia.csv");  // FP da fase 2
  float PF3 = readFloatIEEE(32558 - 30001); Serial.printf("FP Fase 3:    %.3f\n", PF3); salvarNoSD("PF3", PF3, "/fator_potencia.csv");  // FP da fase 3
  float PFt = readFloatIEEE(32560 - 30001); Serial.printf("FP Total:     %.3f\n", PFt); salvarNoSD("PFt", PFt, "/fator_potencia.csv");  // FP total

  // --- ÂNGULOS DE FASE (arquivo angulos.csv) --- exibidos em graus
  float ang1 = readFloatIEEE(32570 - 30001); Serial.printf("Ângulo U1-I1: %.1f °\n", ang1); salvarNoSD("ang_U1_I1", ang1, "/angulos.csv");   // Defasagem tensão x corrente, fase 1
  float ang2 = readFloatIEEE(32572 - 30001); Serial.printf("Ângulo U2-I2: %.1f °\n", ang2); salvarNoSD("ang_U2_I2", ang2, "/angulos.csv");   // Defasagem tensão x corrente, fase 2
  float ang3 = readFloatIEEE(32574 - 30001); Serial.printf("Ângulo U3-I3: %.1f °\n", ang3); salvarNoSD("ang_U3_I3", ang3, "/angulos.csv");   // Defasagem tensão x corrente, fase 3
  float angt = readFloatIEEE(32576 - 30001); Serial.printf("Ângulo Total: %.1f °\n", angt); salvarNoSD("ang_total", angt, "/angulos.csv");   // Defasagem total
  float ang12 = readFloatIEEE(32578 - 30001); Serial.printf("Ângulo U1-U2: %.1f °\n", ang12); salvarNoSD("ang_U1_U2", ang12, "/angulos.csv"); // Ângulo entre as tensões U1 e U2
  float ang23 = readFloatIEEE(32580 - 30001); Serial.printf("Ângulo U2-U3: %.1f °\n", ang23); salvarNoSD("ang_U2_U3", ang23, "/angulos.csv"); // Ângulo entre as tensões U2 e U3
  float ang31 = readFloatIEEE(32582 - 30001); Serial.printf("Ângulo U3-U1: %.1f °\n", ang31); salvarNoSD("ang_U3_U1", ang31, "/angulos.csv"); // Ângulo entre as tensões U3 e U1

  // --- FREQUÊNCIA (arquivo frequencia.csv) ---
  float freq = readFloatIEEE(32584 - 30001); Serial.printf("Frequência:   %.2f Hz\n", freq); salvarNoSD("Frequência", freq, "/frequencia.csv"); // Frequência da rede elétrica

  // --- THD DAS CORRENTES (arquivo thd_correntes.csv) --- distorção harmônica em %
  float thd_i1 = readFloatIEEE(32588 - 30001); Serial.printf("THD I1:       %.2f%%\n", thd_i1); salvarNoSD("THD_I1", thd_i1, "/thd_correntes.csv"); // THD da corrente 1
  float thd_i2 = readFloatIEEE(32590 - 30001); Serial.printf("THD I2:       %.2f%%\n", thd_i2); salvarNoSD("THD_I2", thd_i2, "/thd_correntes.csv"); // THD da corrente 2
  float thd_i3 = readFloatIEEE(32592 - 30001); Serial.printf("THD I3:       %.2f%%\n", thd_i3); salvarNoSD("THD_I3", thd_i3, "/thd_correntes.csv"); // THD da corrente 3
  // --- THD DAS TENSÕES (arquivo thd_tensoes.csv) ---
  float thd_u1 = readFloatIEEE(32594 - 30001); Serial.printf("THD U1:       %.2f%%\n", thd_u1); salvarNoSD("THD_U1", thd_u1, "/thd_tensoes.csv");   // THD da tensão 1
  float thd_u2 = readFloatIEEE(32596 - 30001); Serial.printf("THD U2:       %.2f%%\n", thd_u2); salvarNoSD("THD_U2", thd_u2, "/thd_tensoes.csv");   // THD da tensão 2
  float thd_u3 = readFloatIEEE(32598 - 30001); Serial.printf("THD U3:       %.2f%%\n", thd_u3); salvarNoSD("THD_U3", thd_u3, "/thd_tensoes.csv");   // THD da tensão 3

  // --- TEMPO LIGADO E ENERGIA ACUMULADA ---
  float runtime = readFloatIEEE(32480 - 30001); Serial.printf("Tempo ligado: %.2f h\n", runtime); salvarNoSD("Runtime", runtime, "/runtime.csv"); // Horas de funcionamento do medidor (runtime.csv)
  float kwh1 = readFloatIEEE(32638 - 30001); Serial.printf("kWh Fase 1:   %.2f kWh\n", kwh1); salvarNoSD("Kwh1", kwh1, "/energia.csv");             // Energia ativa acumulada (energia.csv)
  float kwh2 = readFloatIEEE(32640 - 30001); Serial.printf("kWh Fase 2:   %.2f kWh\n", kwh2); salvarNoSD("Kwh2", kwh2, "/energia.csv");             // Energia ativa acumulada (segundo contador)
  float kvarh3 = readFloatIEEE(32642 - 30001); Serial.printf("kvarh 3:      %.2f kvarh\n", kvarh3); salvarNoSD("Kvarh3", kvarh3, "/energia.csv");   // Energia reativa acumulada (primeiro contador)
  float kvarh4 = readFloatIEEE(32644 - 30001); Serial.printf("kvarh 4:      %.2f kvarh\n", kvarh4); salvarNoSD("Kvarh4", kvarh4, "/energia.csv");   // Energia reativa acumulada (segundo contador)

  Serial.println("════════════════════════════════════════════════"); // Linha separadora de fim do ciclo

  // --- ENVIO AO NODE-RED VIA MQTT ---
  if (client.connected()) {                    // Só envia se o MQTT estiver conectado
    enviarParte1();                            // Publica o JSON da parte 1 (tensões e correntes)
    delay(100);                                // Pequena pausa de 100 ms entre os envios para não sobrecarregar o broker
    enviarParte2();                            // Publica o JSON da parte 2 (potências, FP, ângulos, THD, energia)
    Serial.println("2 PARTES JSON ENVIADAS COM SUCESSO!"); // Confirma o envio no serial
  } else {
    Serial.println("MQTT DESCONECTADO - Dados apenas no Serial/SD"); // Sem MQTT: os dados ficam só no serial e no cartão SD
  }
}
