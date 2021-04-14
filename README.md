# webrelay
WebRelay

```
/*  Web Relay v1.0
 *  mariobaldi.py@gmail.com 
 *  17/04/2021
 *  per Nicola 
 *  
 *  Endpoints:
 *  http://IP:PORT/relayXon dove X è intero compreso tra 0 e 3 inclusi
 *  http://IP:PORT/setup?X=Y dove X è il parametro da settare e Y è il valore
 *    X = 1 IP HOST
 *    X = 2 IP GATEWAY
 *    X = 3 IP SUBNET MASK
 *    X = 4 TEMPO CHIUSURA RELAY BISTABILE IN SECONDI
 *    X = 5 DHCP, 1 ABILITATO, 2 CONFIGURAZIONE STATICA DA PARAMETRI SOPRA
 *  http://IP:PORT/reset riavvia il dispositivo caricando le configurazioni settate
 *  http://IP:PORT/relayXoff dove X è intero compreso tra 0 e 3 inclusi, in caso di disattivazione
 *    prematura del relay se è settato un tempo di chiusura lungo (es. luce che si accende per 60 secondi
 *    e voglio spegnerla prima)
 */
```
