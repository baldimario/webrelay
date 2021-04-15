/*  Web Relay v1.0
 *  mariobaldi.py@gmail.com 
 *  15/04/2021
 *  per Nicola 
 *  
 *  Endpoints:
 *  http://IP:PORT/relayXon dove X è intero compreso tra 1 e 4 inclusi
 *  http://IP:PORT/setup?X=Y dove X è il parametro da settare e Y è il valore
 *    X = 1 IP HOST
 *    X = 2 IP GATEWAY
 *    X = 3 IP SUBNET MASK
 *    X = 4 TEMPO CHIUSURA RELAY BISTABILE IN SECONDI
 *    X = 5 DHCP, 1 ABILITATO, 2 CONFIGURAZIONE STATICA DA PARAMETRI SOPRA
 *  http://IP:PORT/reset riavvia il dispositivo caricando le configurazioni settate
 *  http://IP:PORT/relayXoff dove X è intero compreso tra 1 e 4 inclusi, in caso di disattivazione
 *    prematura del relay se è settato un tempo di chiusura lungo (es. luce che si accende per 60 secondi
 *    e voglio spegnerla prima)
 */

/* 
 * Libreria per SPI (Serial Peripheral Interface) 
 * serve a controllare dispositivi seriali, in questo
 * caso l'enc28j60, il controller ethernet
 */
#include <SPI.h> 

/*
 * La libreria Ethernet si occupa di offrire una libreria 
 * basilare che offre un web client, un web server, un http 
 * proxy, gestione di protocollo ucp e udp, ntp, dns, dhcp, 
 * telnet, ecc
 */
#include <Ethernet.h> 

/*
 * La libreria Ticker permette di eseguire funzioni in modo
 * temporizzato in modo non boccante, così da continuare ad
 * effettuare task, insomma, simula un multitasking
 */
#include <Ticker.h> //Scheduler

/*
 * La libreria CRCx permette di calcolare facilmente i codici
 * a ridondanza ciclica ad 8 e 16 bit. Serve per verificare
 * che i dati nella eeprom dell'arduino (cofnigurazione ip ecc)
 * siano coerenti con la configurazione settata altrimenti si
 * ripristina ai valori di default impostati nel firmware
 */
#include <CRCx.h> 

/*
 * Questa libreria espone un'interfaccia alla EEPROM dell'arduino
 * dove scrivere i dati delle configurazioni, in particolare la
 * configurazione ip ed i parametri dei tempi di chiusura dei relay
 */
#include <EEPROM.h> //EEP

#define EEPROM_OFFSET 0x00 //indirizzo da cui cominciare ad utilizzare la EEPROM
#define RESET_PIN A5 //pin reset al quale è possibile collegare un pulsante a massa per resettare
#define FORCE_RESET 0 //flag di debug per forzare il reset (serve per testare il firmware mentre si programma)
#define N_RELAYS 4 //definisce quanti relay ci sono sul modull
#define DELTA_T_MS 50 //millisecondi di risoluzione per calcolo tempo
#define HTTP_PORT 80 //porta webserver
#define RELAY_ON HIGH
#define RELAY_OFF LOW

/*
 * MAC address dispositivo, piccola chicca: è Nicola in esadecimale
 */
byte mac[] = { 0x4E, 0x69, 0x63, 0x6f, 0x6c, 0x61 };

//seguono i prototipi delle funzioni impelementate dopo il loop()
void timeTickerRoutine(void);
void writeConfig(void);
void setupConfig(void);
void software_reboot(void);
void decodeIP(String, byte[]);
void initEthernet(void);
void printForm(EthernetClient, String, int, char*); //uso l'override per disegnare il form i vari tipi di dato
void printForm(EthernetClient, String, int, uint32_t);
void printForm(EthernetClient, String, int, bool);
void resetForm(EthernetClient);
void relayControls(EthernetClient);

/*
 * Definisco una struttura dati per le configurazioni
 */
struct MConfig   {
  bool dhcp;
  byte ip[4]; //ip statico host
  byte gateway[4]; //gateway sottorete
  byte subnet[4]; //subnet mask sottorete
  uint32_t closetime; //tempo di chiusura
  uint16_t crc; //codice a ridondanza ciclica per validare la correttezza della struttura dati
};
typedef struct MConfig MConfig;

EthernetServer server(HTTP_PORT); //classe per webserver ethernet
Ticker timeTicker(timeTickerRoutine, DELTA_T_MS, 0, MILLIS); //scheduler per calcolo del tempo

String http_request_buffer; //variabile per contenere la richiesta http

int pins[] = {4, 5, 6, 7}; //pin dei relay sulla shield
unsigned long int times[] = {0, 0, 0, 0}; //timer countdown dei relay
MConfig configs; //variabile che mantiene le configurazioni lette dalla eeprom al boot

MConfig defaultConfigs { //configurazioni di default
  true, {10, 0, 0, 252}, {10, 10, 0, 254}, {255, 255, 255, 0}, 1
};

/*
 * La funzione setup viene eseguita al boot
 */
void setup() {  
  delay(100); //aspetto 100 millisecondi per inizializzare risorse come la EEPROM
  
  pinMode(RESET_PIN, INPUT_PULLUP); //setto il piedino di reset in configurazione pullup interno
  digitalWrite(RESET_PIN, HIGH); //alimento il piedino di reset così che se buttato a massa farò reset
  
  Serial.begin(9600); //inizializzo la seriale per debug
  
  setupConfig(); //leggo le configurazioni dalla eeprom

  /*
   * Setto tutti i pin relay come output
   */
  for (int i = 0; i < N_RELAYS; i++) 
    pinMode(pins[i], OUTPUT);
  
  /*
   * Alcune shield relay hanno i livelli logici invertiti, dunque alzo tutti i 
   * valori logici dei pin per spegnere tutti i relay al boot
   */
  for (int i = 0; i < N_RELAYS; i++) 
    digitalWrite(pins[i], RELAY_OFF);

  initEthernet();

  if (Ethernet.hardwareStatus() == EthernetNoHardware) { //verifico se la shield ethernet è attaccata
    Serial.println("Ethernet shield not attached");
    while (true) { //aspetto all'infinito
      delay(1); // non ha senso andare avanti
    }
  }
  
  if (Ethernet.linkStatus() == LinkOFF) { //se l'ethernet non linka
    Serial.println("Ethernet cable not connected"); //stampo che non ho il cavo attaccato
  }
  
  server.begin(); //inizializzo il webserver
  
  Serial.print("server is at "); //scrivo sulla seriale via USB
  Serial.println(Ethernet.localIP()); //l'ip dell'arduino per facilitare il debug

  timeTicker.start(); //avvio la schedulazione del contatore per il tempo
}

/*
 * La funzione loop viene eseguita continuamente, al suo termine viene rieseguita, il paradigma
 * di programmazione si chiama non per caso SuperLoop, tipico degli embedded systems di basso livello
 */
void loop() {
  timeTicker.update(); //aggiorno la routine di conteggio temporale

  EthernetClient client = server.available(); //mi metto in ascolto di un client
  if (client) { //se un client si connette
    while(client.connected()) { //finchè il client è connesso
      if(client.available()) { //verifico la disponibilità del client
        bool config_changed = false; //variabile per mantenere memoria del cambiamento di configurzione;
        char c = client.read(); //leggo un carattere dal client
        
        Serial.write(c);
        if(http_request_buffer.length() < 256) {
          http_request_buffer += c; //concateno il carattere letto alla stringa buffer
        }

        if(c == '\n') { //se il carattere è \n, come fa protocollo http, la richiesta GET è finita
          Serial.println(http_request_buffer); //stampo la stringa per debug

          if(http_request_buffer.substring(5, 10) == String("relay")) { //se richiedo l'endpoint "relay"
            Serial.println("RELAY"); //stampo per debug l'endpoint
            byte pin_idx = http_request_buffer.charAt(10) - 49; //48 ascii 0 -1 per l'index del pin
            byte value = http_request_buffer.substring(11, 13) == String("on") ? RELAY_ON : RELAY_OFF; //per il valore acceso o spento

            if(pin_idx >= 0 && pin_idx < N_RELAYS) { //se il relay è tra 0 e N_RELAY
              byte pin = pins[pin_idx]; //recupero il pin dall'indice

              if(value) { //se devo accenderlo
                times[pin_idx] = configs.closetime*1000; //imposto il tempo per il timer di quel relay
              }
              else {
                times[pin_idx] = 0; //resetto il timer per quel pin
              }

              digitalWrite(pin, value ? RELAY_ON : RELAY_OFF); //setto il valore del pin che pilota il relay in logica inverita
            }
          }
  
          if(http_request_buffer.substring(5, 10) == String("setup")) { //se richiedo l'endpoint SETUP
            Serial.println("SETUP"); //stampo per debug l'endpoint
  
            String data = http_request_buffer.substring(11).substring(0, http_request_buffer.length()-22);
            Serial.println(data); //stampo il dato letto
  
            byte sep = data.indexOf('='); //indice separatore
            byte param = data.substring(0, sep).toInt(); //parametro
            String value = data.substring(sep+1); //valore
  
            //debuggo parametro e valore letti
            Serial.print(param);
            Serial.print(" -> ");
            Serial.println(value); 
  
            //in base al parametro da cambiare setto la struttura di configurazione
            switch(param) {
              case 1:
                decodeIP(value, configs.ip); //scrivo l'ip letto nella configurazione attuale
                config_changed = true; //flaggo il cambiamento delle configurazioni
              break;
              case 2:
                decodeIP(value, configs.gateway); //come sopra per il gateway
                config_changed = true;
              break;
              case 3:
                decodeIP(value, configs.subnet); //idem per la subnet mask
                config_changed = true;
              break;
              case 4: //così per il tempo di apertura dei relay bistabili
                configs.closetime = value.toInt(); //se setto il tempo leggo e salvo nelle conf attuali
                config_changed = true;
              break; 
              case 5: //idem ancora per dhcp
                configs.dhcp = value == "1" ? true : false;
                config_changed = true;
              break;
            }
  
            http_request_buffer = ""; //resetto la stringa della richiesta correttamente elaborata;
            writeConfig(); //scrivo la configurazione sulla EEEPROM
            delay(100); //attendo 100 millisecondi per rilasciare le risorse della EEPROM
          }
  
          if(http_request_buffer.substring(5, 10) == String("reset")) { //se richiedo l'endpoint reset
            delay(1); //buona norma
            client.stop(); //chiudo la connessione col client
            digitalWrite(RESET_PIN, LOW); //resetto il raspberry col reset fisico
            initEthernet(); //se non reetta reinizializzo la ethernet
            software_reboot(); //nel caso estremo uso l'assembly per resettare tutto il firmware
          }
  
          //Ritorno un 200 OK da protocollo HTTP al client
          client.print("HTTP/1.1 200 OK\nContent-Type: text/html\n\n" \
            "<html><head><title>Web Relay</title></head><body><h3>WebRelay</h3><hr>");
  
          if(config_changed) //se la configurazione flaggata cambiata
            client.print("Config changed<hr>"); //stampo un feedback

          printForm(client, "DHCP", 5, configs.dhcp);
          printForm(client, "IP", 1, configs.ip);
          printForm(client, "Gateway", 2, configs.gateway);
          printForm(client, "Subnet", 3, configs.subnet);
          printForm(client, "Contact Time (s)", 4, configs.closetime);
          resetForm(client);
          client.print("<hr>");
          relayControls(client);
          
          client.print("</body></html>");
  
          delay(1); //buona norma
          client.stop(); //chiudo la connessione col client
          http_request_buffer = ""; //resetto la stringa della richiesta correttamente elaborata;
          delay(100); //attendo 100 millisecondi prima di accettare un'altra richiesta altrimenti in caso di alimentazione scarsa il firmware può incepparsi e non servire la pagina web
        }
      }
    }
  }
}

/*
 * 
 */
void initEthernet(void) {
  if(!configs.dhcp) 
    Ethernet.begin(mac, configs.ip, configs.gateway, configs.subnet);
  else
    Ethernet.begin(mac);
}

/*
 * Questa funzione decodifica da stringa ad ip, tutto il
 * passaggio dei valori di ritorno avviene per referenza
 */
void decodeIP(String value, byte aip[]) {
    int sep = 0, i = 0;
    do {
      sep = value.indexOf('.', 0);
      Serial.println(value);
      Serial.println(value.substring(0, sep).toInt());
      aip[i] = value.substring(0, sep).toInt();

      value = value.substring(sep+1, value.length());
      i++;
    } while(i < 4);
}

/*
 * Questa funzione, prende le configurazioni attuali, calcola il nuovo CRC a 16 bit e le salva nella EEPROM
 */
void writeConfig(void) {
  configs.crc = crcx::crc16((uint8_t*)&configs, sizeof(configs) - sizeof(configs.crc));
  EEPROM.put(EEPROM_OFFSET, configs);
}

/*
 * Questa funzione legge le configurazioni nella EEPROM, controlla il CRC a 16 bit e nel caso fallisca
 * il controllo carica le configurazioni di default
 */
void setupConfig(void) {
  EEPROM.get(EEPROM_OFFSET, configs); //Leggo i dati dalla EEPROM a partire dall'offset definito nella struttura dati
  
  int crc = crcx::crc16((uint8_t*)&configs, sizeof(configs) - sizeof(configs.crc)); //calcolo il CRC a 16 bit dalla configurazione letta
    
  if ( configs.crc != crc || FORCE_RESET) { //se il crc della struttura letta è diversa da quello calcoalto o sto debuggando con un reset forzato della configurazione
    Serial.println("Uninitialized EEPROM"); //debuggo che l'eeprom non è inizializzata
    configs = defaultConfigs; //copio la configurazione attuale in quella di default
    configs.crc = crcx::crc16((uint8_t*)&configs, sizeof(MConfig) - sizeof(configs.crc)); //calcolo il nuovo CRC16
    EEPROM.put(EEPROM_OFFSET, configs); //scrivo la configurazione resettata nella EEPROM
  }
  else //se invece il controllo del CRC16 ha successo
    Serial.println("Initialized EEPROM"); //scrivo per debug che la EEPROM è inizializzata
}

/*
 * 
 */
void timeTickerRoutine(void) {
  for(int i = 0; i < N_RELAYS; i++) { //per ognuno dei relay
    if(times[i] > 0) { //se il tempo residuo di attivazione è definito positivo
      times[i] -= DELTA_T_MS; //sottraggo al timer del relay il delta T (risoluzione ticker)

      if(times[i] <= 0) { //se il timer del relay i-esimo scende a 0 o meno
        times[i] = 0; //imposto a 0 il timer (potrebbe essere negativo)
        digitalWrite(pins[i], RELAY_OFF); //setto basso (SPENGO) il relay
      }
    }
  }
}

/*
 * funzioni per disegnare form di configurazione
 */
void printForm(EthernetClient client, String text, int param, byte value[]) {
  char* ipbuf = malloc(sizeof(char)*15);
  sprintf(ipbuf, "%d.%d.%d.%d", value[0], value[1], value[2], value[3]);

  client.print("<form method=\"GET\" action=\"/setup\">");
  client.print(text);
  client.print(": <input type=\"text\" name=\"");
  client.print(param);
  client.print("\" value=\"");
  client.print(ipbuf);
  client.println("\"><input type=\"submit\" value=\"Set\"></form>");  
}

void printForm(EthernetClient client, String text, int param, uint32_t value) {
  client.print("<form method=\"GET\" action=\"/setup\">");
  client.print(text);
  client.print(": <input type=\"text\" name=\"");
  client.print(param);
  client.print("\" value=\"");
  client.print(value);
  client.println("\"><input type=\"submit\" value=\"Set\"></form>");  
}

void printForm(EthernetClient client, String text, int param, bool value) {
  client.print("<form method=\"GET\" action=\"/setup\">");
  client.print(text);
  client.print(": <input type=\"text\" name=\"");
  client.print(param);
  client.print("\" value=\"");
  client.print(value);
  client.println("\"><input type=\"submit\" value=\"Set\"></form>");  
}

void resetForm(EthernetClient client) {
  client.print("<form method=\"GET\" action=\"/reset\">");
  client.print("<input type=\"submit\" value=\"Reset\"></form>");  
}

void relayControls(EthernetClient client) {
  for(int i = 1; i <= N_RELAYS; i++) {
    client.print("<form method=\"GET\" style=\"display: inline\" action=\"/relay");
    client.print(i);
    client.print("on\"><input type=\"submit\" value=\"");
    client.print(i);
    client.print("\"></form>");  
  }
}

/*
 * piccolo trucchetto, in C posso scrivere codice assembly, quindi
 * un JMP all'indirizzo di memoria 0x0 dell'area codice è tecnicamente
 * un soft-reset del firmware dell'arduino
 */
void software_reboot() {
  asm volatile ("  jmp 0");
}
