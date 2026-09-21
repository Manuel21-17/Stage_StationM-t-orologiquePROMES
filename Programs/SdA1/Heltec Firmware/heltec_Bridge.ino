/* =============================================================================
   heltec_puente.ino  --  Heltec WiFi LoRa 32 V4 (HTIT-WB32LAF) y V3
   -----------------------------------------------------------------------------
   Puente entre el Arduino Mega 2560 y el resto del sistema:

       [Mega 2560] --I2C--> [ESTA PLACA] --LoRa--> [otra Heltec V4 o V3]
                                  ^
                                  |  comandos por WiFi (ESP-NOW)
                                  |  y tambien por LoRa y por USB
                            [otra Heltec]

   QUE HACE
     1. Es MAESTRO I2C. Lee del Mega (esclavo 0x2A) la linea de texto
            Lec:A0:512,A1:0, ... ,A15:37
        al mismo ritmo que marca comVel, porque esta placa obedece el mismo
        comando comVel que el Mega.
     2. Reemite esa linea por LoRa a otra Heltec (V4 o V3, da igual).
     3. Recibe comandos por WiFi (ESP-NOW) mandados por otra Heltec, y tambien
        por LoRa y por su propio USB. Los aplica y se los pasa al Mega por I2C.

   COMANDOS (los mismos del Mega, terminados en salto de linea):
     comVel:###    -> periodo de muestreo en ms. Se aplica AQUI (ritmo de
                      lectura I2C) y se reenvia al Mega.
     comSerial:1/0 -> volcado de lecturas por USB. Se aplica AQUI y se reenvia.
     comRel:1/0    -> rele del Mega. Solo se reenvia.

   -----------------------------------------------------------------------------
   COMPATIBILIDAD V3 / V4

   Los pines del radio SX1262 son identicos en las dos placas (NSS 8, SCK 9,
   MOSI 10, MISO 11, RST 12, BUSY 13, DIO1 14), y aqui se declaran de forma
   explicita, asi que este sketch compila y funciona igual con "WiFi LoRa
   32(V3)" o con "WiFi LoRa 32(V4)" seleccionada en el IDE.

   OJO CON UNA TRAMPA: las macros SDA y SCL del nucleo NO valen entre placas.
   La variante V4 las define como GPIO3 y GPIO4; la V3, como GPIO41 y GPIO42.
   Por eso aqui el I2C se abre con numeros de pin explicitos y nunca con SDA
   ni SCL. Lo mismo con SPI.begin().

   PINES ELEGIDOS PARA EL ENLACE CON EL MEGA
     GPIO5 -> SDA        GPIO6 -> SCL        GPIO4 -> DRDY (opcional)
   Segun la tabla 2.2.2 del datasheet de la V4 (Rev 1.4), GPIO4, GPIO5 y GPIO6
   son ADC1 + TOUCH y nada mas: no chocan con el radio (8-14), el OLED (17/18,
   tabla 2.2.3), la medida de bateria (1), su control ADC_Ctrl (37), el GNSS
   (38-42) ni los pines de arranque del S3 (0, 3, 45, 46). En la V3 tambien
   estan libres y sacados a tira.

   *** NO USAR GPIO7 EN LA V4 ***
   La tabla 2.2.2 lo declara "GPIO7, ADC1_CH6, TOUCH7, VFEM_Control": controla
   el modulo de front-end de RF, o sea el amplificador que da los 28 dBm de la
   version de alta potencia. En la V3 ese pin esta libre, pero aqui conmutarlo
   al ritmo del reloj I2C dejaria el LoRa inservible. Por eso el bus se movio
   de 6/7 a 5/6.

   -----------------------------------------------------------------------------
   *** NIVELES LOGICOS: HACE FALTA ADAPTADOR ***
   El Mega trabaja a 5 V y este ESP32-S3 NO tolera 5 V. Usa un adaptador de
   nivel I2C bidireccional (BSS138 / TXS0102 / PCA9306): lado Mega con pull-up
   de 4k7 a 5 V, lado Heltec con pull-up de 4k7 a 3,3 V. Y GND comun.
   Si cableas DRDY, esa linea tambien necesita bajar de 5 V a 3,3 V.

   *** EL LIMITE DE VERDAD: EL CICLO DE TRABAJO DEL LoRa ***
   NO se puede reemitir por LoRa cada muestra. La linea Lec: ocupa ~138 bytes
   y a SF7/BW125 tarda unos 230 ms en el aire. En Europa la banda de 868 MHz
   esta limitada al 1% de ciclo de trabajo, o sea que despues de transmitir hay
   que callar unas 100 veces ese tiempo: ~23 segundos.

   Por eso hay DOS ritmos independientes:
     - comVel  -> cada cuanto se LEE al Mega por I2C (puede ser 100 ms).
     - INTERVALO_LORA_MS -> cada cuanto se TRANSMITE la ultima linea leida
       (por defecto 30 s), ademas con un guardian que calcula el tiempo real
       en el aire de cada paquete y bloquea la siguiente emision.
   Si necesitas mas cadencia por radio: baja el factor de ensanchado (SF) o
   acorta la linea; no toques el guardian.
   ============================================================================= */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <RadioLib.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_system.h>

/* ========================= Configuracion ================================== */

/* --- Enlace I2C con el Mega --- */
#define PIN_SDA               5
#define PIN_SCL               6
#define PIN_DRDY              4
#define USAR_DRDY             0      // 1 = usa GPIO4 para saber que hay muestra
                                     // nueva en vez de fiarse solo del reloj
#define DIR_MEGA           0x2A
#define FREQ_I2C         400000UL    // 400 kHz. Baja a 100000 si el adaptador
                                     // de nivel o los cables dan problemas.

/* --- Formato de la linea del Mega --- */
#define LINEA_CAP           160      // 5 lecturas I2C de 32 bytes
#define TROZO_I2C            32
#define N_TROZOS            (LINEA_CAP / TROZO_I2C)
#define REG_COMANDO        0xF0      // registro de escritura de comandos

/* --- Radio LoRa (identico en V3 y V4) --- */
#define PIN_NSS               8
#define PIN_DIO1             14
#define PIN_RST              12
#define PIN_BUSY             13
#define PIN_SCK               9
#define PIN_MISO             11
#define PIN_MOSI             10

#define LORA_MHZ          869.525    // Sub-banda EU "g3" (869,4-869,65 MHz),
                                     // que permite 10% de ciclo de trabajo.
                                     // En 868,0-868,6 solo se permite el 1%,
                                     // que era lo que forzaba los 30 s entre
                                     // tramas. TIENE que coincidir con el
                                     // heltec_receptor.
#define LORA_BW_KHZ       125.0
#define LORA_SF               7      // 7 = rapido y corto; 12 = lento y lejos
#define LORA_CR               5      // 4/5
#define LORA_SYNC          0x12      // 0x12 red privada, 0x34 LoRaWAN publica
#define LORA_POTENCIA_DBM    14      // limite legal EU en 868,0-868,6 MHz
#define LORA_PREAMBULO        8
#define LORA_TCXO_V         1.8
#define LORA_LDO          false

#define INTERVALO_LORA_MS   2500UL   // con el 10% de la banda g3 se puede bajar
                                     // de 30 s a 2,5 s (ver nota arriba)
#define PRIMERA_EMISION_MS  3000UL   // la PRIMERA sale pronto, para no tener
                                     // que esperar a ver si el enlace va
#define CICLO_TRABAJO_PCT    10      // 10% en la sub-banda g3. Es el guardian
                                     // legal: si bajas INTERVALO_LORA_MS por
                                     // debajo de lo que toca, el te frena.
#define ESCUCHAR_LORA         1      // 1 = tambien acepta comandos por LoRa

/* --- WiFi / ESP-NOW --- */
#define CANAL_WIFI            1      // el mismo en las dos Heltec
#define ECO_ESPNOW            1      // 1 = devuelve los acuses por ESP-NOW
#define ENVIAR_POR_ESPNOW     1      // 1 = manda TAMBIEN cada lectura por WiFi
                                     // en cuanto la lee del Mega.
                                     //
                                     // ESTA es la via de tiempo real: ESP-NOW
                                     // no tiene ciclo de trabajo, asi que sale
                                     // al ritmo de comVel (100 ms por defecto)
                                     // en vez de cada 2,5 s como el LoRa.
                                     // El alcance es de ~100-200 m; el LoRa
                                     // queda de respaldo para la distancia.

/* --- OLED integrado (SSD1315, 128x64, compatible SSD1306) ---
   Pines segun la tabla 2.2.3 del datasheet de la V4 (Rev 1.4); iguales en V3.
   La pantalla se alimenta del rail Vext: GPIO36 a nivel BAJO lo enciende.

   *** OJO: LA PANTALLA VA EN Wire1, NO EN Wire ***
   En esta placa el bus Wire ya esta ocupado hablando con el Mega en GPIO5/6.
   El ESP32-S3 tiene DOS perifericos I2C, asi que el OLED se lleva al segundo
   (Wire1) y cada repintado deja de interferir con la lectura del Mega.
   En el receptor no hacia falta porque alli Wire estaba libre.               */
#define USAR_OLED             1
#define PIN_OLED_SDA         17
#define PIN_OLED_SCL         18
#define PIN_OLED_RST         21
#define PIN_VEXT             36
#define DIR_OLED           0x3C
#define REFRESCO_OLED_MS   1000UL    // repintado de fondo, para el "hace Ns"
#define PERIODO_MIN_OLED_MS 200UL    // no repinta mas rapido que esto aunque
                                     // lleguen datos: volcar el buffer de 1 kB
                                     // cuesta ~23 ms, y con comVel:50 el loop
                                     // se pasaria la vida dibujando. 200 ms ya
                                     // se ve instantaneo.

/* --- Varios --- */
#define BAUDIOS          115200UL
#define INTERVALO_ESTADO_MS 30000UL  // linea de vida por USB

/* ========================= Limites de comVel ============================== */
const uint32_t INTERVALO_MIN_MS = 5UL;
const uint32_t INTERVALO_MAX_MS = 3600000UL;
const uint32_t INTERVALO_DEF_MS = 100UL;

/* ========================= Estado global ================================== */
SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY);

uint32_t intervaloMs  = INTERVALO_DEF_MS;   // comVel, compartido con el Mega
bool     volcarSerial = true;              // comSerial local

char     lineaCruda[LINEA_CAP + 1];         // lo que llega del Mega
char     ultimaLinea[LINEA_CAP + 1];        // ultima linea valida
uint16_t ultimaLen      = 0;
bool     hayLineaNueva  = false;

/* Contadores para la linea de vida. */
uint32_t nLineasOK = 0, nFallosI2C = 0, nEmisiones = 0, nComandos = 0;
uint32_t nEnviosNow = 0, nFallosNow = 0;

/* --- Pantalla --- */
const uint8_t N_CANALES_LEC = 16;            // A0..A15 en la linea Lec:
#if USAR_OLED
Adafruit_SSD1306 oled(128, 64, &Wire1, -1);  // Wire1: Wire es del Mega
bool     oledVivo = false;
uint16_t canales[N_CANALES_LEC];
bool     hayDatos = false;
uint32_t proximoRefrescoOled = 0;
uint32_t ultimoPintado = 0;
#endif
float    ultimoRssi = 0;

/* --- Maquina de estados de la lectura I2C (un trozo por vuelta) --- */
enum EstadoI2C : uint8_t { I2C_OCIOSO, I2C_LEYENDO };
EstadoI2C estadoI2C   = I2C_OCIOSO;
uint8_t   trozoActual = 0;
uint32_t  proximaLectura = 0;

/* --- Maquina de estados del radio --- */
enum EstadoLora : uint8_t { LORA_ESCUCHA, LORA_TRANSMITIENDO };
volatile bool dio1Disparado = false;
EstadoLora estadoLora   = LORA_ESCUCHA;
uint32_t   proximaEmision = 0;
uint32_t   bloqueoTxHasta = 0;      // guardian de ciclo de trabajo
uint32_t   tiempoAireMs   = 0;
uint32_t   inicioTx       = 0;

uint32_t proximoEstado = 0;

/* --- Cola de comandos pendientes de reenviar al Mega --- */
const uint8_t COLA_CMD = 4;
const uint8_t CMD_MAX  = 40;
char    colaCmd[COLA_CMD][CMD_MAX];
uint8_t colaCmdN = 0;

/* --- Buffers de acumulacion de comandos, uno por origen --- */
char cmdUsb[CMD_MAX];    uint8_t cmdUsbLen  = 0;
char cmdNow[CMD_MAX];    uint8_t cmdNowLen  = 0;
char cmdLora[CMD_MAX];   uint8_t cmdLoraLen = 0;

/* --- Cola que llena la interrupcion de ESP-NOW --- */
const uint16_t NOW_CAP = 256;
volatile char     nowBuf[NOW_CAP];
volatile uint16_t nowCabeza = 0, nowCola = 0;

const uint8_t DIFUSION[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* Prototipos */
void ejecutarComando(char *linea, const char *origen);

/* ===========================================================================
   Diagnostico de reinicios
   ---------------------------------------------------------------------------
   El ESP32 guarda por que se reinicio la ultima vez, y esto lo traduce a algo
   legible. Es LA pista para distinguir un fallo de alimentacion de un fallo
   de codigo, que se parecen mucho desde fuera pero se arreglan al reves.
   =========================================================================== */
const char *razonReset() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "encendido normal";
    case ESP_RST_EXT:       return "reset externo (boton RST)";
    case ESP_RST_SW:        return "reinicio por software";
    case ESP_RST_PANIC:     return "PANIC: excepcion en el codigo";
    case ESP_RST_INT_WDT:   return "watchdog de interrupciones";
    case ESP_RST_TASK_WDT:  return "watchdog de tarea: el loop se bloqueo";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "BROWNOUT: se hundio la tension";
    case ESP_RST_DEEPSLEEP: return "salida de sueno profundo";
    default:                return "desconocida";
  }
}

/* ===========================================================================
   Utilidades de texto
   =========================================================================== */
const char *coincide(const char *linea, const char *clave) {
  while (*clave) {
    if (tolower((unsigned char)*linea) != tolower((unsigned char)*clave)) return NULL;
    linea++; clave++;
  }
  return linea;
}

bool leerBooleano(const char *valor, bool &destino) {
  while (*valor == ' ') valor++;
  if (*valor == '1') { destino = true;  return true; }
  if (*valor == '0') { destino = false; return true; }
  return false;
}

/* ===========================================================================
   Pantalla OLED integrada
   ---------------------------------------------------------------------------
   Misma rejilla que en heltec_receptor: la fuente por defecto de Adafruit_GFX
   es monoespaciada de 6x8, o sea 21 columnas y 8 filas. Cuatro valores de 4
   digitos mas separadores son 19 caracteres.

       Mega #1247 err:0     <- lineas leidas del Mega y fallos de I2C
        512    0 1023   37  <- A0  A1  A2  A3
        ...                 <- A4  A5  A6  A7
        ...                 <- A8  A9  A10 A11
        ...                 <- A12 A13 A14 A15
       A0>A15   LoRa 12s    <- orden de lectura y cuenta atras de la emision
   =========================================================================== */
#if USAR_OLED

/* Extrae los 16 numeros de "Lec:A0:512,A1:0,...". El memcpy tiene que estar
   en TODOS los caminos de exito: la primera version salia antes de copiar y
   la pantalla enseñaba 16 ceros pasara lo que pasara. */
bool parsearLec(const char *linea, uint16_t *destino, uint8_t n) {
  if (n == 0 || n > N_CANALES_LEC) return false;
  if (strncmp(linea, "Lec:", 4) != 0) return false;

  uint16_t tmp[N_CANALES_LEC];
  const char *p = linea + 4;
  bool completa = false;

  for (uint8_t c = 0; c < n; c++) {
    const char *dosp = strchr(p, ':');          // el de "A7:"
    if (!dosp) return false;
    tmp[c] = (uint16_t)strtoul(dosp + 1, NULL, 10);

    const char *coma = strchr(dosp, ',');
    if (!coma) { completa = (c == n - 1); break; }   // el ultimo no lleva coma
    p = coma + 1;
    if (c == n - 1) completa = true;
  }

  if (!completa) return false;
  memcpy(destino, tmp, (size_t)n * sizeof(uint16_t));
  return true;
}

/* Espera acotada, solo durante el arranque. No es delay() y nunca corre
   dentro del loop. */
void esperaArranque(uint32_t ms) {
  uint32_t t = millis();
  while ((millis() - t) < ms) { /* nada */ }
}

void iniciarOled() {
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);       // Vext a BAJO = pantalla alimentada

  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, LOW);
  esperaArranque(20);
  digitalWrite(PIN_OLED_RST, HIGH);
  esperaArranque(50);

  Wire1.begin(PIN_OLED_SDA, PIN_OLED_SCL, 400000UL);

  /* Los dos "false": reset ya se lo hemos dado nosotros, y periphBegin a false
     es IMPRESCINDIBLE porque si no la libreria llama a Wire1.begin() sin
     argumentos y se carga los pines que acabamos de fijar. */
  if (!oled.begin(SSD1306_SWITCHCAPVCC, DIR_OLED, false, false)) {
    Serial.println(F("# ERR no encuentro el OLED en 0x3C"));
    oledVivo = false;
    return;
  }
  oledVivo = true;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setTextWrap(false);           // sin esto una linea larga descuadra todo
  oled.setCursor(0, 0);
  oled.println(F("Heltec puente"));
  oled.println();
  oled.println(F("Esperando al Mega..."));
  oled.printf("I2C %d/%d  0x%02X\n", PIN_SDA, PIN_SCL, DIR_MEGA);
  oled.display();

  Serial.println(F("# OLED listo (en Wire1, GPIO17/18)"));
}

void pintarOled() {
  if (!oledVivo) return;
  ultimoPintado = millis();

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  if (!hayDatos) {
    oled.setCursor(0, 0);
    oled.println(F("Heltec puente"));
    oled.println();
    oled.println(F("Sin datos del Mega"));
    oled.printf("fallos I2C: %lu\n", (unsigned long)nFallosI2C);
    oled.display();
    return;
  }

  oled.setCursor(0, 0);
  oled.printf("Mega #%lu err:%lu",
              (unsigned long)nLineasOK, (unsigned long)nFallosI2C);

  for (uint8_t fila = 0; fila < 4; fila++) {
    oled.setCursor(0, 12 + fila * 10);
    for (uint8_t col = 0; col < 4; col++) {
      oled.printf("%4u", canales[fila * 4 + col]);
      if (col < 3) oled.print(' ');
    }
  }

  int32_t faltaTx = (int32_t)(proximaEmision - millis());
  if (faltaTx < 0) faltaTx = 0;
  oled.setCursor(0, 56);
  oled.printf("A0>A15   LoRa %lds", (long)(faltaTx / 1000));

  oled.display();
}
#endif  /* USAR_OLED */

/* ===========================================================================
   Reenvio de comandos al Mega
   ---------------------------------------------------------------------------
   No se envian desde donde se reciben, sino que se encolan y salen cuando el
   bus I2C esta libre: asi nunca se interrumpe una lectura a medias.
   =========================================================================== */
void encolarParaMega(const char *cmd) {
  if (colaCmdN >= COLA_CMD) {
    Serial.println(F("# AVISO: cola de comandos al Mega llena, se descarta"));
    return;
  }
  snprintf(colaCmd[colaCmdN], CMD_MAX, "%s\n", cmd);
  colaCmdN++;
}

void enviarColaAlMega() {
  if (colaCmdN == 0 || estadoI2C != I2C_OCIOSO) return;

  const char *cmd = colaCmd[0];
  Wire.beginTransmission(DIR_MEGA);
  Wire.write((uint8_t)REG_COMANDO);
  Wire.write((const uint8_t *)cmd, strlen(cmd));
  uint8_t err = Wire.endTransmission(true);

  if (err != 0) {
    nFallosI2C++;
    Serial.printf("# ERR no pude mandar '%.*s' al Mega (I2C %u)\n",
                  (int)strlen(cmd) - 1, cmd, err);
  }

  /* Se saca de la cola pase lo que pase: si el Mega no responde, reintentar
     en bucle solo taparia el problema real. */
  for (uint8_t i = 1; i < colaCmdN; i++) memcpy(colaCmd[i - 1], colaCmd[i], CMD_MAX);
  colaCmdN--;
}

/* ===========================================================================
   Salida de acuses
   =========================================================================== */
void responder(const char *texto) {
  Serial.println(texto);
#if ECO_ESPNOW
  esp_now_send(DIFUSION, (const uint8_t *)texto, strlen(texto));
#endif
}

/* ===========================================================================
   Parser de comandos, compartido por USB, ESP-NOW y LoRa
   =========================================================================== */
void ejecutarComando(char *linea, const char *origen) {
  while (*linea == ' ' || *linea == '\t') linea++;
  if (*linea == '\0') return;

  char resp[96];
  const char *v;
  nComandos++;

  if ((v = coincide(linea, "comvel:")) != NULL) {
    uint32_t ms = strtoul(v, NULL, 10);
    if (ms == 0) {
      snprintf(resp, sizeof(resp), "ERR comVel valor invalido [%s]", origen);
      responder(resp);
      return;
    }
    if (ms < INTERVALO_MIN_MS) ms = INTERVALO_MIN_MS;
    if (ms > INTERVALO_MAX_MS) ms = INTERVALO_MAX_MS;
    intervaloMs    = ms;                 // ritmo de lectura de ESTA placa
    proximaLectura = millis();
    encolarParaMega(linea);              // y el Mega muestrea a lo mismo
    snprintf(resp, sizeof(resp), "OK comVel=%lu ms [%s]",
             (unsigned long)intervaloMs, origen);
    responder(resp);
    return;
  }

  if ((v = coincide(linea, "comserial:")) != NULL) {
    bool estadoCmd;
    if (!leerBooleano(v, estadoCmd)) {
      snprintf(resp, sizeof(resp), "ERR comSerial espera 0 o 1 [%s]", origen);
      responder(resp);
      return;
    }
    volcarSerial = estadoCmd;            // volcado local
    encolarParaMega(linea);              // y el del Mega
    snprintf(resp, sizeof(resp), "OK comSerial=%u [%s]",
             (unsigned)(volcarSerial ? 1 : 0), origen);
    responder(resp);
    return;
  }

  if (coincide(linea, "comrel:") != NULL) {
    /* No hay rele en la Heltec: esto es cosa del Mega, se reenvia tal cual.
       El Mega valida el argumento y contesta por su propio puerto serie. */
    encolarParaMega(linea);
    snprintf(resp, sizeof(resp), "OK %s reenviado al Mega [%s]", linea, origen);
    responder(resp);
    return;
  }

  snprintf(resp, sizeof(resp), "ERR comando desconocido: %.32s [%s]", linea, origen);
  responder(resp);
}

/* Acumula caracteres hasta el salto de linea. */
void acumular(char c, char *buf, uint8_t &len, const char *origen) {
  if (c == '\n' || c == '\r') {
    if (len > 0) { buf[len] = '\0'; ejecutarComando(buf, origen); len = 0; }
    return;
  }
  if (c < 32 || c > 126) return;
  if (len < CMD_MAX - 1) buf[len++] = c;
  else len = 0;
}

/* ===========================================================================
   ESP-NOW: comandos que manda otra Heltec por WiFi
   ---------------------------------------------------------------------------
   La llamada de retorno corre en la tarea de WiFi: solo copia bytes a una
   cola. El parseo se hace en el loop, como todo lo demas.
   =========================================================================== */
void alRecibirEspNow(const esp_now_recv_info_t *info, const uint8_t *datos, int len) {
  (void)info;
  for (int i = 0; i < len; i++) {
    uint16_t sig = (uint16_t)((nowCabeza + 1) % NOW_CAP);
    if (sig == nowCola) return;          // cola llena: se descarta el resto
    nowBuf[nowCabeza] = (char)datos[i];
    nowCabeza = sig;
  }
  /* Si el remitente no puso salto de linea, lo damos por terminado aqui:
     un paquete ESP-NOW es un mensaje completo por definicion. */
  uint16_t sig = (uint16_t)((nowCabeza + 1) % NOW_CAP);
  if (sig != nowCola) { nowBuf[nowCabeza] = '\n'; nowCabeza = sig; }
}

void atenderEspNow() {
  for (;;) {
    if (nowCola == nowCabeza) break;
    char c = nowBuf[nowCola];
    nowCola = (uint16_t)((nowCola + 1) % NOW_CAP);
    acumular(c, cmdNow, cmdNowLen, "wifi");
  }
}

void iniciarEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(CANAL_WIFI, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println(F("# ERR no arranco ESP-NOW"));
    return;
  }
  esp_now_register_recv_cb(alRecibirEspNow);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, DIFUSION, 6);
  peer.channel = CANAL_WIFI;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.printf("# ESP-NOW en canal %d, MAC de esta placa: %s\n",
                CANAL_WIFI, WiFi.macAddress().c_str());
}

/* ===========================================================================
   Radio
   =========================================================================== */
void IRAM_ATTR alDio1() { dio1Disparado = true; }

bool iniciarRadio() {
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);

  int16_t st = radio.begin(LORA_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
                           LORA_SYNC, LORA_POTENCIA_DBM, LORA_PREAMBULO,
                           LORA_TCXO_V, LORA_LDO);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("# ERR radio.begin devolvio %d\n", st);
    /* -706 / -707 suelen ser la tension del TCXO: prueba LORA_TCXO_V a 0
       (sin TCXO) o a 3.3. */
    return false;
  }

  radio.setDio1Action(alDio1);
  radio.startReceive();
  estadoLora = LORA_ESCUCHA;
  Serial.printf("# LoRa %.1f MHz SF%d BW%.0f kHz, %d dBm\n",
                LORA_MHZ, LORA_SF, LORA_BW_KHZ, LORA_POTENCIA_DBM);
  return true;
}

void emitirPorLora() {
  uint32_t ahora = millis();

  if (estadoLora != LORA_ESCUCHA) return;
  if (!hayLineaNueva) return;
  if ((int32_t)(ahora - proximaEmision) < 0) return;
  if ((int32_t)(ahora - bloqueoTxHasta) < 0) return;   // guardian del 1%

  tiempoAireMs = (uint32_t)(radio.getTimeOnAir(ultimaLen) / 1000UL);

  radio.standby();
  int16_t st = radio.startTransmit((uint8_t *)ultimaLinea, ultimaLen);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("# ERR startTransmit %d\n", st);
    radio.startReceive();
    proximaEmision = ahora + INTERVALO_LORA_MS;
    return;
  }

  inicioTx      = ahora;
  estadoLora    = LORA_TRANSMITIENDO;
  hayLineaNueva = false;
}

void atenderRadio() {
  uint32_t ahora = millis();

  /* Salvavidas: si DIO1 no llega (radio colgado), no nos quedamos mudos
     para siempre. Con dias de marcha esto acaba pasando alguna vez.        */
  if (estadoLora == LORA_TRANSMITIENDO &&
      (ahora - inicioTx) > (tiempoAireMs * 3 + 500)) {
    Serial.println(F("# AVISO: transmision sin DIO1, reinicio la escucha"));
    radio.standby();
    radio.startReceive();
    estadoLora     = LORA_ESCUCHA;
    proximaEmision = ahora + INTERVALO_LORA_MS;
    return;
  }

  if (!dio1Disparado) return;
  dio1Disparado = false;

  if (estadoLora == LORA_TRANSMITIENDO) {
    radio.finishTransmit();
    nEmisiones++;

    /* Ciclo de trabajo: tras emitir hay que callar (100/PCT - 1) veces el
       tiempo que se ha estado en el aire. Con PCT=1 son 99 veces. */
    bloqueoTxHasta = ahora + tiempoAireMs * (100UL / CICLO_TRABAJO_PCT);
    proximaEmision = ahora + INTERVALO_LORA_MS;

    radio.startReceive();
    estadoLora = LORA_ESCUCHA;
    return;
  }

#if ESCUCHAR_LORA
  /* Estabamos escuchando: ha llegado algo. Solo aceptamos comandos. */
  uint16_t len = radio.getPacketLength();
  if (len > 0 && len < CMD_MAX * 2) {
    char entrante[CMD_MAX * 2 + 1];
    int16_t st = radio.readData((uint8_t *)entrante, len);
    if (st == RADIOLIB_ERR_NONE) {
      entrante[len] = '\0';
      ultimoRssi = radio.getRSSI();
      for (uint16_t i = 0; i < len; i++) acumular(entrante[i], cmdLora, cmdLoraLen, "lora");
      if (cmdLoraLen > 0) { acumular('\n', cmdLora, cmdLoraLen, "lora"); }
    }
  }
  /* startReceive() limpia las banderas de interrupcion y reengancha la
     escucha, tambien cuando el paquete se ha descartado sin leerlo. */
  radio.startReceive();
#else
  radio.startReceive();
#endif
}

/* ===========================================================================
   Lectura del Mega por I2C, un trozo de 32 bytes por vuelta del loop
   =========================================================================== */
bool leerTrozo(uint8_t trozo) {
  uint8_t desplazamiento = (uint8_t)(trozo * TROZO_I2C);

  Wire.beginTransmission(DIR_MEGA);
  Wire.write(desplazamiento);
  if (Wire.endTransmission(true) != 0) return false;

  if (Wire.requestFrom((int)DIR_MEGA, (int)TROZO_I2C) != TROZO_I2C) return false;
  for (uint8_t i = 0; i < TROZO_I2C; i++) {
    lineaCruda[desplazamiento + i] = (char)Wire.read();
  }
  return true;
}

void procesarLinea() {
  lineaCruda[LINEA_CAP] = '\0';

  if (strncmp(lineaCruda, "Lec:", 4) != 0) {
    nFallosI2C++;
    Serial.println(F("# ERR la linea del Mega no empieza por Lec:"));
    return;
  }

  /* El Mega rellena con ceros; el texto acaba en el salto de linea. */
  char *fin = strchr(lineaCruda, '\n');
  uint16_t len = fin ? (uint16_t)(fin - lineaCruda + 1) : (uint16_t)strlen(lineaCruda);
  if (len < 8 || len > LINEA_CAP) {
    nFallosI2C++;
    Serial.println(F("# ERR longitud de linea absurda"));
    return;
  }

  memcpy(ultimaLinea, lineaCruda, len);
  ultimaLinea[len] = '\0';
  ultimaLen        = len;
  hayLineaNueva    = true;
  nLineasOK++;

#if ENVIAR_POR_ESPNOW
  /* Salida inmediata por WiFi: sin ciclo de trabajo que respetar, la lectura
     llega al receptor al ritmo de comVel. La copia por LoRa sigue su propio
     calendario mas lento, en emitirPorLora().                                */
  if (esp_now_send(DIFUSION, (const uint8_t *)ultimaLinea, ultimaLen) == ESP_OK) {
    nEnviosNow++;
  } else {
    nFallosNow++;      // cola de ESP-NOW llena: se pierde esta, no se reintenta
  }
#endif

#if USAR_OLED
  /* Pintado inmediato con cada lectura nueva, con un suelo de tiempo para que
     un comVel muy corto no deje al loop dibujando sin parar. */
  if (parsearLec(ultimaLinea, canales, N_CANALES_LEC)) {
    hayDatos = true;
    if ((millis() - ultimoPintado) >= PERIODO_MIN_OLED_MS) {
      pintarOled();
      proximoRefrescoOled = millis() + REFRESCO_OLED_MS;
    }
  }
#endif

  if (volcarSerial) Serial.write((const uint8_t *)ultimaLinea, len);
}

void atenderI2C() {
  uint32_t ahora = millis();

  if (estadoI2C == I2C_OCIOSO) {
    if ((int32_t)(ahora - proximaLectura) < 0) return;
#if USAR_DRDY
    if (digitalRead(PIN_DRDY) == LOW) {      // el Mega aun no tiene nada nuevo
      proximaLectura = ahora + 1;
      return;
    }
#endif
    trozoActual = 0;
    estadoI2C   = I2C_LEYENDO;
  }

  /* Un trozo por vuelta: cada lectura I2C son ~0,8 ms a 400 kHz, asi que el
     loop nunca se queda quieto mas de eso y el radio sigue atendido.        */
  if (!leerTrozo(trozoActual)) {
    nFallosI2C++;
    estadoI2C      = I2C_OCIOSO;
    proximaLectura = millis() + intervaloMs;
    return;
  }

  trozoActual++;
  if (trozoActual >= N_TROZOS) {
    estadoI2C = I2C_OCIOSO;
    procesarLinea();

    /* Reengancha sin arrastrar el retraso si un ciclo se pasa de largo. */
    proximaLectura += intervaloMs;
    if ((int32_t)(millis() - proximaLectura) >= 0) proximaLectura = millis() + intervaloMs;
  }
}

/* ===========================================================================
   Linea de vida por USB
   =========================================================================== */
void lineaDeVida() {
  uint32_t ahora = millis();
  if ((int32_t)(ahora - proximoEstado) < 0) return;
  proximoEstado = ahora + INTERVALO_ESTADO_MS;

  int32_t faltaTx = (int32_t)(proximaEmision - ahora);
  int32_t faltaDc = (int32_t)(bloqueoTxHasta - ahora);
  if (faltaTx < 0) faltaTx = 0;
  if (faltaDc < 0) faltaDc = 0;

  Serial.printf("# estado: lineas=%lu fallosI2C=%lu | wifi=%lu (fallos %lu) | "
                "lora=%lu comandos=%lu comVel=%lu ms | proxima emision LoRa en "
                "%ld s (bloqueo %ld s) | rssi=%.0f dBm | heap=%lu B\n",
                (unsigned long)nLineasOK, (unsigned long)nFallosI2C,
                (unsigned long)nEnviosNow, (unsigned long)nFallosNow,
                (unsigned long)nEmisiones, (unsigned long)nComandos,
                (unsigned long)intervaloMs,
                (long)(faltaTx / 1000), (long)(faltaDc / 1000), ultimoRssi,
                (unsigned long)ESP.getFreeHeap());
}

/* ===========================================================================
   setup / loop
   =========================================================================== */
void setup() {
  Serial.setTxBufferSize(1024);      // holgura para no bloquear al imprimir
  Serial.begin(BAUDIOS);

  /* Espera corta y acotada a que el USB se establezca. No bloquea para
     siempre si no hay nadie escuchando al otro lado.                       */
  uint32_t inicio = millis();
  while (!Serial && (millis() - inicio) < 2000) { /* sin delay */ }

  Serial.println(F("# Heltec puente: Mega (I2C) -> LoRa, comandos por WiFi/LoRa/USB"));

  /* Lo primero que se imprime, para que un bucle de reinicios se explique solo
     en vez de tener que adivinarlo. */
  Serial.printf("# ARRANQUE. Causa del ultimo reset: %s | heap libre %lu B\n",
                razonReset(), (unsigned long)ESP.getFreeHeap());

#if USAR_DRDY
  pinMode(PIN_DRDY, INPUT);
#endif

  Wire.begin(PIN_SDA, PIN_SCL, FREQ_I2C);
  Serial.printf("# I2C maestro en SDA=%d SCL=%d a %lu Hz, Mega en 0x%02X\n",
                PIN_SDA, PIN_SCL, (unsigned long)FREQ_I2C, DIR_MEGA);

#if USAR_OLED
  iniciarOled();
#endif

  iniciarEspNow();
  iniciarRadio();

  memset(lineaCruda, 0, sizeof(lineaCruda));
  memset(ultimaLinea, 0, sizeof(ultimaLinea));

  uint32_t ahora  = millis();
  proximaLectura  = ahora;
  proximaEmision  = ahora + PRIMERA_EMISION_MS;
  proximoEstado   = ahora + INTERVALO_ESTADO_MS;
#if USAR_OLED
  proximoRefrescoOled = ahora + REFRESCO_OLED_MS;
#endif

  Serial.printf("# comVel inicial %lu ms | emision LoRa cada %lu s\n",
                (unsigned long)intervaloMs,
                (unsigned long)(INTERVALO_LORA_MS / 1000));
  Serial.println(F("# comandos: comVel:###  comSerial:0|1  comRel:0|1"));
}

void loop() {
  /* Comandos por USB. */
  while (Serial.available() > 0) {
    acumular((char)Serial.read(), cmdUsb, cmdUsbLen, "usb");
  }

  atenderEspNow();      // comandos por WiFi
  atenderRadio();       // fin de emision o comando por LoRa
  atenderI2C();         // lectura del Mega, un trozo por vuelta
  enviarColaAlMega();   // reenvio de comandos, solo con el bus libre
  emitirPorLora();      // emision, si toca y el ciclo de trabajo lo permite
  lineaDeVida();

#if USAR_OLED
  /* Repintado de fondo: mantiene viva la cuenta atras del LoRa y refresca la
     pantalla aunque el Mega deje de responder. */
  if ((int32_t)(millis() - proximoRefrescoOled) >= 0) {
    proximoRefrescoOled = millis() + REFRESCO_OLED_MS;
    pintarOled();
  }
#endif

  /* Cesion de CPU al planificador. OJO, esto NO es de los delay() que
     quitamos: aquellos eran esperas activas que se comian el procesador sin
     hacer nada. vTaskDelay LIBERA la CPU durante un tick para que corran las
     tareas de menor prioridad -- la de reposo, que es la que alimenta el
     watchdog, y las de mantenimiento de WiFi.

     Sin esto, el loop de Arduino (prioridad 1) puede dejar sin turno a la
     tarea de reposo (prioridad 0) indefinidamente, y eso acaba en reinicio
     por watchdog o en inestabilidad del WiFi. Es la practica estandar en
     ESP32 y cuesta 1 ms de cada vuelta.                                     */
  vTaskDelay(1);
}
