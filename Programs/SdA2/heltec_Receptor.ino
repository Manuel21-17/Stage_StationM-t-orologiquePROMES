/*
  heltec_Final.ino  --  Heltec WiFi LoRa 32 V4 (HTIT-WB32LAF) y V3

  Nace de heltec_davis.ino, que se conserva ENTERO: todos los parametros
  de radio de este sketch estan medidos sobre esta pareja concreta de
  equipos, no heredados de ninguna tabla publica. No se toca nada de eso.

  Lo que se le anade es el tratamiento de lo que ENTRA por SPI desde el
  Portenta, que antes se descartaba, y una pantalla de estado.

  QUE HACE ESTA PLACA

    1. Escucha la estacion Davis Vantage Pro2 Plus en FSK con salto de
       frecuencia, y arma una linea JSON por trama.

    2. Manda esa linea a la Raspberry por USB serie, que es de donde la
       recoge ingesta_davis.py para guardarla y subirla a la nube.

    3. Habla con el Portenta H7 por SPI, como esclavo, EN LOS DOS
       SENTIDOS:
         - le devuelve por CIPO la misma linea JSON que manda a la Pi;
         - y ahora tambien lee lo que el Portenta le envia por COPI
           ($ADC, $RTC y mensajes manuales) y lo reenvia a la Pi.

       Ese segundo sentido es lo unico que faltaba: el bloque de entrada
       ya llegaba por DMA a bufferEntradaSPI, pero nadie lo miraba.

    4. Muestra en la OLED si estan entrando datos del Davis y del
       Portenta. Es un extra: ver USAR_OLED mas abajo.

    5. Recibe por ESP-NOW las lineas "Lec:A0:###,..." que difunde la otra
       Heltec (heltec_puente.ino) y las reenvia tambien a la Pi.

  POR QUE ESP-NOW Y NO LoRa

  La otra Heltec manda esas lineas por dos vias a la vez, y este sketch
  usa la de WiFi. No es un apano: es la unica que cabe aqui.

  El SX1262 es UN SOLO radio y solo puede estar en un modo a la vez:

      Davis       ->  FSK, saltando entre 868.060 y 868.540 MHz cada
                      2.5625 s, sincronizado con el ISS.
      Otra Heltec ->  LoRa, fijo en 869.525 MHz, SF7 BW125.

  Escuchar LoRa significa dejar de seguir el salto del Davis, y volver a
  engancharlo cuesta tramas. Es lo mismo que ya esta escrito en la
  cabecera de heltec_lora_emisor.ino: "su radio no puede a la vez cazar
  tramas del Davis con salto de frecuencia y emitir LoRa".

  ESP-NOW en cambio va por el WiFi del ESP32-S3, que es un radio DISTINTO
  e independiente del SX1262. Las dos cosas conviven sin tocarse.

  Y no hay que cambiar nada en la otra placa: heltec_puente.ino ya
  difunde cada lectura por ESP-NOW nada mas leerla del Mega, a la MAC de
  difusion y en el canal CANAL_WIFI. Alli lo llaman "salida inmediata por
  WiFi", frente a la copia por LoRa que va mas lenta por el ciclo de
  trabajo. Aqui solo hay que escuchar.

  SOBRE EL ALCANCE

  A los ~100 m previstos entre las dos placas, ESP-NOW normal funciona
  con vision directa y se vuelve dudoso con obstaculos por medio. Si se
  pierden lineas, la solucion es el modo de largo alcance de Espressif:

      esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);

  Da del orden de 10 dB mas de sensibilidad, pero TIENE que ponerse en
  LAS DOS placas: si solo una lo usa, no se entienden.

  ---------------------------------------------------------------------
  A partir de aqui, la cabecera original de heltec_davis.ino.
  ---------------------------------------------------------------------
*/

/*
  Heltec WiFi LoRa 32 V4  --  Davis Vantage Pro2 Plus (EU 868 MHz)

  PLACA: HTIT-WB32LAF. Ese codigo figura en la tabla de variantes del
  datasheet de la V4 (Rev 1.4, sep 2025) como el modelo de banda alta:
  863-928 MHz y 28 dBm de transmision. Lleva ESP32-S3R2 con 2 MB de PSRAM
  y 16 MB de flash, frente a los 8 MB sin PSRAM de la V3.

  COMPATIBILIDAD V3 / V4 -- VERIFICADA CONTRA EL DATASHEET, NO SUPUESTA

  Heltec declara "form factor and pin compatibility with WiFi LoRa 32 V3",
  y comprobando el mapa de pines de la V4 uno por uno se confirma para todo
  lo que usa este sketch:

    Radio SX1262, identico en V4:
      GPIO8 NSS, GPIO9 SCK, GPIO10 MOSI, GPIO11 MISO,
      GPIO12 RST, GPIO13 BUSY, GPIO14 DIO1.

    Enlace SPI con el Portenta, todos disponibles en las tiras de la V4:
      GPIO33 (J2 pin 12), GPIO47 (J2 pin 13), GPIO3 (J3 pin 14) y
      GPIO35 (J2 pin 10).

  La V4 anade conectores de GNSS y de panel solar que la V3 no tenia. El
  GNSS ocupa GPIO38 a GPIO42, y el solar es solo alimentacion: NINGUNO
  choca con los cuatro pines del enlace SPI. Por eso este sketch funciona
  en V4 sin cambiar una sola linea de codigo.

  En el IDE conviene seleccionar "WiFi LoRa 32(V4)" si tu paquete de
  tarjetas ya la lista. Con la V3 seleccionada tambien compila y funciona,
  pero desaprovechas la mitad de la flash y no habilita la PSRAM (que este
  sketch no necesita).

  USB: LA V4 NO LLEVA CHIP USB-SERIE

  La V3 tiene un CP2102 y sale en la Pi como /dev/ttyUSB0. La V4 no: su
  USB-C va directo al USB interno del ESP32-S3, y en la Pi aparece como
  "303a:1001 Espressif USB JTAG/serial debug unit", en /dev/ttyACM*.

  Por eso hay que compilar con

      Herramientas > USB CDC On Boot > Enabled
      Herramientas > USB Mode        > Hardware CDC and JTAG

  Con "Disabled", que es lo que trae el paquete por defecto, Serial sale
  por los pines TX/RX (GPIO43/44) y NO por el USB. La Pi sigue viendo la
  placa, porque ese USB lo gestiona el hardware, pero no le llega ni una
  linea. Paso el 2026-09-11. Mas abajo hay un #error que impide compilar
  para la V4 con la opcion equivocada.

  Lo unico que el datasheet no documenta es la tension del TCXO. Si al
  arrancar sale un error -706 o -707 de beginFSK, mira la nota de TCXO_V
  mas abajo.

  ETAPA 3: receptor definitivo, con salida en tabla ancha.

  TODO LO QUE HAY AQUI ESTA MEDIDO, NO HEREDADO

  Las etapas 1 y 2 determinaron experimentalmente cada parametro sobre esta
  pareja concreta de equipos:

    - Sincronizacion CB 89 en orden directo, carga util con los bits
      INVERTIDOS dentro de cada byte. La asimetria es real y fue lo que mas
      costo encontrar: el sync va derecho y el payload al reves.
    - Transmitter ID del ISS: 1.
    - Cinco canales espaciados 120 kHz, medidos contando tramas validas.
      Salen 17.25 kHz por debajo de la tabla publica de rtldavis, que son
      unas 20 ppm de deriva combinada entre el cristal del ISS y el TCXO
      del Heltec. Sintonizamos donde de verdad se recibe, no donde deberia.
    - Periodo de salto 2.5625 s y orden de salto, deducidos de los tiempos.
    - Pluviometro metrico: 0.2 mm por vuelco.

  Verificado en marcha: 2154 tramas con 0 perdidas en 92 minutos, y 23-24
  tramas por minuto frente a un maximo teorico de 23.4. Captura del 100%.

  DOS SALIDAS, NUNCA A LA VEZ

  SALIDA_TABLA = 1  ->  tabla ancha legible por el monitor serie.
  SALIDA_TABLA = 0  ->  una linea JSON por trama, para ingesta_serial.py.

  SOBRE EL ARRASTRE DE VALORES

  Cada trama trae viento y direccion mas UNA sola magnitud rotatoria; nunca
  llegan la temperatura y la humedad juntas. En la tabla cada hueco se
  rellena con el ultimo valor recibido, que es lo que la hace legible.

  Ese arrastre es de PRESENTACION, no de archivo. Al guardar en la base de
  datos conviene registrar el instante real de cada medida y dejar que
  Grafana haga el relleno al consultar: un valor arrastrado que ya escribiste
  en disco no se puede distinguir despues de uno medido, y eso no tiene
  vuelta atras.

  Por eso la columna EDAD: segundos desde la actualizacion mas antigua de
  la fila. Pequena significa que todo esta fresco; si crece, algo dejo de
  llegar. Hace visible el arrastre en lugar de disimularlo.

  SOBRE LA LLUVIA

  El contador de vuelcos que emite la estacion va de 0 a 127 y da la vuelta.
  Aqui se acumula en milimetros tratando ese salto, PERO el acumulado se
  reinicia al reiniciar la placa. Para el historico de verdad, la Pi debe
  guardar el contador crudo y calcular las diferencias contra la base de
  datos, no contra la memoria del microcontrolador.

  La tasa de lluvia (mensaje 0x5) es el unico campo sin verificar contra
  datos reales, porque no ha llovido durante los ensayos.
*/

#include <RadioLib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include "driver/spi_slave.h"

// Ver "USB: LA V4 NO LLEVA CHIP USB-SERIE" en la cabecera. Sin esto el
// sketch compila, se sube y parece funcionar, pero la Pi no recibe nada.
#if defined(ARDUINO_HELTEC_WIFI_LORA_32_V4) && !ARDUINO_USB_CDC_ON_BOOT
  #error "Activa Herramientas > USB CDC On Boot > Enabled: la V4 no tiene chip USB-serie y sin eso Serial no sale por el USB."
#endif

// --- Pantalla OLED integrada ---------------------------------------------
// 1 = usa la OLED de la placa para ver de un vistazo si entran datos.
// 0 = la desactiva por completo, tambien sus librerias.
//
// NO ES IMPRESCINDIBLE. Refrescarla son unos 25 ms de I2C bloqueante una
// vez por segundo. No deberia costar tramas, porque el SX1262 guarda el
// paquete hasta que se lee, pero si tras activarla ves crecer "perdidas",
// pon esto a 0 y sales de dudas.
//
// Necesita las librerias "Adafruit SSD1306" y "Adafruit GFX Library",
// las dos desde el gestor de librerias del IDE.
#define USAR_OLED 1

#if USAR_OLED
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
#endif

// --- Enlace con la otra Heltec -------------------------------------------
// 1 = escucha por ESP-NOW lo que difunde heltec_puente.ino.
// 0 = lo desactiva; el WiFi ni siquiera se enciende.
//
// OJO: encender el WiFi mete carga de interrupciones que antes no habia, y
// la captura del Davis depende de tiempos. No deberia notarse, porque el
// SX1262 retiene la trama hasta que se lee, pero si al activarlo ves crecer
// "perdidas", esta es la primera pieza que apagar para descartarlo.
#define USAR_ESPNOW 1

#if USAR_ESPNOW
  #include <WiFi.h>
  #include <esp_now.h>
  #include <esp_wifi.h>
#endif

// --- Salida --------------------------------------------------------------
// 0 = una linea JSON por trama, que es lo que espera ingesta_davis.py.
// 1 = tabla ancha legible en el monitor serie. Hay una copia del sketch
//     configurada asi en la carpeta heltec_davis_tabla, por si la quieres
//     sin tocar esta.
const int SALIDA_TABLA = 0;

// --- Lineas que llegan del Portenta --------------------------------------
// Por debajo de esto no puede ser una linea de datos: es ruido del bus.
const int LONGITUD_MINIMA_LINEA = 4;

// --- Enlace con la otra Heltec -------------------------------------------
#if USAR_ESPNOW

// TIENE que coincidir con CANAL_WIFI de heltec_puente.ino.
const int CANAL_WIFI = 1;

// Nombre con el que salen sus lineas hacia la Pi.
const char* ORIGEN_OTRA_HELTEC = "heltec2";

// ESP-NOW no deja pasar de 250 bytes por mensaje. La linea "Lec:" ronda
// los 138, asi que sobra sitio.
const int CAPACIDAD_ESPNOW = 250;

#endif

// --- Pantalla ------------------------------------------------------------
#if USAR_OLED

// La OLED de la V3 y la V4 va por I2C en estos pines, y su alimentacion
// cuelga de Vext: sin encender Vext no contesta al bus y el begin() falla
// sin decir por que.
const int PIN_OLED_SDA = 17;
const int PIN_OLED_SCL = 18;
const int PIN_OLED_RST = 21;
const int PIN_VEXT     = 36;   // activo a nivel BAJO

const int OLED_ANCHO = 128;
const int OLED_ALTO  = 64;
const int DIRECCION_OLED = 0x3C;

const unsigned long PERIODO_PANTALLA_MS = 1000;

/*
 * A partir de cuanto silencio se considera que una fuente ha dejado de
 * hablar.
 *
 * El Davis manda unas 23 tramas por minuto, una cada 2.5 s: 30 s de nada
 * ya es anormal. El Portenta manda su $RTC una vez por segundo pase lo
 * que pase, asi que con 10 s basta.
 */
const unsigned long SILENCIO_DAVIS_MS    = 30000;
const unsigned long SILENCIO_PORTENTA_MS = 10000;
const unsigned long SILENCIO_HELTEC2_MS  = 30000;

#endif
const unsigned long INTERVALO_FILA_MS = 5000;  // una fila cada 5 s
const int FILAS_POR_CABECERA = 20;

// --- Canales, medidos en la etapa 2 --------------------------------------
const float CANALES_MHZ[] = {
  868.060, 868.180, 868.300, 868.420, 868.540
};
const int NUM_CANALES = 5;

// Orden en que la estacion los visita, deducido de los tiempos: entre dos
// canales consecutivos en frecuencia hay 3 saltos, luego cada salto avanza
// 2 posiciones en la lista de arriba.
//
// SI NO ENGANCHA: si "perdidas" crece tanto como "recibidas" y salen muchos
// resincronizados, la secuencia va al reves. Prueba con {0, 3, 1, 4, 2}.
const int SECUENCIA_SALTO[] = {0, 2, 4, 1, 3};

// 2.5625 s = (41 + 0) / 16, con 0 = ID 1. Se guarda en medios milisegundos
// para programar los saltos con aritmetica entera y sin deriva: el salto
// numero n cae en tRef + (n * 5125) / 2 milisegundos.
const unsigned long MEDIO_MS_POR_SALTO = 5125;
const unsigned long MARGEN_MS = 400;
const int FALLOS_PARA_RESYNC = 8;

const int ID_ESTACION = 1;
const float MM_POR_VUELCO = 0.2;

const char* ID_DISPOSITIVO = "davis1";
// Cada 30 s aunque no entre ni una trama. Es el latido en el que se apoya
// el vigilante de ingesta_davis.py: si el puerto serie muere en silencio,
// la ausencia de estas lineas es lo unico que lo delata. Con 30 s aqui y
// 60 s alla, el vigilante tiene el doble de margen y no salta en falso.
const unsigned long INTERVALO_ESTADO_MS = 30000;

// --- Parametros de radio, verificados en la etapa 1 ----------------------
const uint8_t SYNC_DAVIS[2]  = {0xCB, 0x89};
const float VELOCIDAD_KBPS   = 19.2;
const float DESVIACION_KHZ   = 9.5;
const float ANCHO_BANDA_KHZ  = 58.6;
const int   PREAMBULO_BITS   = 16;
const int   LONGITUD_TRAMA   = 8;

// --- Pines del radio ------------------------------------------------------
// Cotejados con el mapa de pines del datasheet de la V4: coinciden con los
// de la V3 uno por uno, asi que estos siete valores sirven en ambas placas.
const int PIN_NSS  = 8;
const int PIN_DIO1 = 14;
const int PIN_RST  = 12;
const int PIN_BUSY = 13;
const int PIN_SCK  = 9;
const int PIN_MISO = 11;
const int PIN_MOSI = 10;

const float TCXO_V   = 1.8;
const bool  USAR_LDO = false;

SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY);

// --- Enlace SPI con el Portenta H7 ---------------------------------------
//
// El Portenta es el MAESTRO y el Heltec el ESCLAVO. En la misma transaccion
// en la que el Portenta saca su paquete $ADC por COPI, el Heltec le devuelve
// por CIPO la ultima linea JSON del Davis. No cuesta ninguna transaccion
// adicional: se aprovecha la que ya se hacia.
//
//   Señal   Portenta H7            Heltec V4
//   -----   --------------------   ---------
//   CS      D7  / PI0 / SPI1 CS    GPIO33
//   MOSI    D8  / PC3 / SPI1 COPI  GPIO3    (entra al Heltec)
//   CLK     D9  / PI1 / SPI1 CK    GPIO47
//   MISO    D10 / PC2 / SPI1 CIPO  GPIO35   (sale del Heltec)
//
// Ademas hace falta GND comun entre las dos placas.
//
// OJO CON DOS PINES:
//   GPIO35 es el pin del LED blanco, confirmado en la tabla del header J2
//   de la V4 (pin 10). Este sketch no lo usa como LED, asi que queda libre
//   para el SPI, pero cuenta con que el LED parpadeara con los datos y con
//   que su resistencia carga un poco la linea. No lo uses para nada mas.
//   GPIO3 es pin de strapping (seleccion de JTAG). Como entrada despues del
//   arranque no da problema; simplemente no lo fuerces durante un reset.
const int PIN_SPI_CS   = 33;
const int PIN_SPI_MOSI = 3;
const int PIN_SPI_CLK  = 47;
const int PIN_SPI_MISO = 35;

// El radio ya ocupa FSPI (SPI2) a traves del objeto SPI de Arduino, asi que
// el esclavo tiene que ir en el otro periferico de proposito general.
#define HOST_SPI_ESCLAVO SPI3_HOST

// Tiene que coincidir EXACTAMENTE con TAMANO_PAQUETE_SPI del Portenta.
// Son 192 y no 128 porque la trama del pluviometro genera una linea JSON de
// 144 bytes: con bloques de 128 se cortaria y habria que descartarla.
const int TAM_BLOQUE_SPI = 192;

// Buffers que toca el DMA: tienen que estar alineados a palabra.
WORD_ALIGNED_ATTR uint8_t bufferSalidaSPI[TAM_BLOQUE_SPI];
WORD_ALIGNED_ATTR uint8_t bufferEntradaSPI[TAM_BLOQUE_SPI];

// Linea en espera de ser copiada al buffer de salida. Existe para no
// escribir NUNCA sobre bufferSalidaSPI mientras el DMA lo esta leyendo: la
// copia solo se hace entre transacciones.
char lineaPendienteSPI[TAM_BLOQUE_SPI];
bool hayLineaPendienteSPI = false;

spi_slave_transaction_t transaccionSPI;
bool transaccionSPIEnCurso = false;
bool esclavoSPIListo = false;
unsigned long transaccionesSPI = 0;

// Linea JSON que se construye en memoria antes de publicarla. Antes se
// imprimia a trozos directamente al puerto serie; ahora se arma primero
// para poder mandar exactamente la misma al Portenta.
char lineaJson[TAM_BLOQUE_SPI];
int  lineaJsonLen = 0;

/*
 * Linea "$MET,..." que se le ofrece al Portenta, distinta de la JSON.
 *
 * Cada consumidor recibe el formato que entiende: la Pi lee la JSON por
 * el puerto serie y la aplicacion del movil lee esta. Traducir aqui, que
 * es donde estan los datos ya decodificados, evita que el Portenta tenga
 * que parsear JSON solo para reenviarlo.
 */
char lineaMet[TAM_BLOQUE_SPI];

// --- Magnitudes ----------------------------------------------------------
// Un hueco por variable, con el instante de su ultima actualizacion. Eso es
// lo que permite arrastrar el valor y a la vez saber si esta rancio.
enum {
  M_TEMP, M_HUM, M_VIENTO, M_RACHA, M_DIR,
  M_UV, M_SOLAR, M_BAT, M_RSSI, N_MAGNITUDES
};

float         valor[N_MAGNITUDES];
unsigned long instante[N_MAGNITUDES];
bool          visto[N_MAGNITUDES];

// La lluvia va aparte: no es una lectura instantanea sino un acumulado que
// se construye a partir de las diferencias del contador de vuelcos.
int   ultimaCuenta = -1;
float lluviaMm = 0.0;

// --- Estado del enganche -------------------------------------------------
volatile bool tramaLista = false;

bool sincronizado = false;
int  posSecuencia = 0;
unsigned long tRef = 0;
unsigned long saltosDesdeRef = 0;
int  fallosSeguidos = 0;

unsigned long ultimaFila = 0;
unsigned long ultimoEstado = 0;
int filasImpresas = 0;

unsigned long recibidas = 0;
unsigned long perdidas = 0;
unsigned long descartadas = 0;

// --- Lo que entra del Portenta -------------------------------------------
// Copia del bloque recibido, con terminador propio. Se saca del buffer del
// DMA antes de encolar la siguiente transaccion.
char bloqueDelPortenta[TAM_BLOQUE_SPI];

unsigned long lineasDelPortenta = 0;
unsigned long bloquesDescartados = 0;

// Instante de la ultima linea buena de cada fuente, para saber si siguen
// vivas. 0 = todavia no ha llegado ninguna.
unsigned long ultimaLineaPortenta = 0;
unsigned long ultimaTramaDavis = 0;

// --- Lo que entra de la otra Heltec --------------------------------------

/*
 * Estos tres contadores viven fuera del #if a proposito: asi la linea de
 * estado y la pantalla pueden nombrarlos sin condicionales por medio. Con
 * USAR_ESPNOW a 0 se quedan en cero y ya esta.
 */
unsigned long lineasDeOtraHeltec = 0;
unsigned long mensajesEspNowPerdidos = 0;
unsigned long ultimaLineaOtraHeltec = 0;

#if USAR_ESPNOW

/*
 * El aviso de ESP-NOW no se ejecuta en loop(), sino en la tarea de WiFi.
 * Alli no conviene ponerse a escribir por el puerto serie ni a tocar nada
 * largo, asi que solo se deja la linea aqui y loop() la recoge. Es el
 * mismo reparto que hace el Portenta con las escrituras BLE.
 */
char mensajeEspNow[CAPACIDAD_ESPNOW + 1];
volatile bool hayMensajeEspNow = false;

bool espNowListo = false;

#endif

#if USAR_OLED
Adafruit_SSD1306 oled(OLED_ANCHO, OLED_ALTO, &Wire, PIN_OLED_RST);

bool pantallaLista = false;
unsigned long ultimaPantalla = 0;
#endif


void IRAM_ATTR alLlegarTrama() {
  tramaLista = true;
}


uint16_t crc16Davis(const uint8_t* datos, int longitud) {
  uint16_t crc = 0x0000;
  for (int i = 0; i < longitud; i++) {
    crc ^= (uint16_t)datos[i] << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}


uint8_t invertirBits(uint8_t b) {
  b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
  b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
  b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
  return b;
}


// --- Construccion de la linea JSON --------------------------------------

void iniciarLineaJson() {
  lineaJson[0] = '\0';
  lineaJsonLen = 0;
}


// Anexa texto formateado a lineaJson sin desbordar nunca el buffer.
void anexarJson(const char* formato, ...) {
  int espacio = (int)sizeof(lineaJson) - lineaJsonLen;
  if (espacio <= 1) {
    return;
  }

  va_list argumentos;
  va_start(argumentos, formato);
  int n = vsnprintf(lineaJson + lineaJsonLen, espacio, formato, argumentos);
  va_end(argumentos);

  if (n < 0) {
    return;
  }

  // vsnprintf devuelve lo que HABRIA escrito, asi que hay que limitarlo.
  lineaJsonLen += (n < espacio) ? n : (espacio - 1);
}


// --- Esclavo SPI ---------------------------------------------------------

// Deja una linea preparada para la proxima lectura del Portenta. No toca el
// buffer del DMA: solo el intermedio.
void prepararLineaSPI(const char* linea) {
  strncpy(lineaPendienteSPI, linea, sizeof(lineaPendienteSPI) - 1);
  lineaPendienteSPI[sizeof(lineaPendienteSPI) - 1] = '\0';
  hayLineaPendienteSPI = true;
}


void encolarTransaccionSPI() {
  memset(&transaccionSPI, 0, sizeof(transaccionSPI));
  transaccionSPI.length = TAM_BLOQUE_SPI * 8;   // la longitud va en BITS
  transaccionSPI.tx_buffer = bufferSalidaSPI;
  transaccionSPI.rx_buffer = bufferEntradaSPI;

  if (spi_slave_queue_trans(HOST_SPI_ESCLAVO, &transaccionSPI, 0) == ESP_OK) {
    transaccionSPIEnCurso = true;
  }
}


void iniciarEsclavoSPI() {
  memset(bufferSalidaSPI, 0, sizeof(bufferSalidaSPI));
  memset(bufferEntradaSPI, 0, sizeof(bufferEntradaSPI));
  lineaPendienteSPI[0] = '\0';

  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_SPI_MOSI;
  bus.miso_io_num = PIN_SPI_MISO;
  bus.sclk_io_num = PIN_SPI_CLK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = TAM_BLOQUE_SPI;

  spi_slave_interface_config_t esclavo = {};
  esclavo.spics_io_num = PIN_SPI_CS;
  esclavo.queue_size = 2;
  esclavo.mode = 0;              // el Portenta usa SPI_MODE0
  esclavo.flags = 0;

  esp_err_t r = spi_slave_initialize(HOST_SPI_ESCLAVO, &bus, &esclavo,
                                     SPI_DMA_CH_AUTO);
  if (r != ESP_OK) {
    if (!SALIDA_TABLA) {
      Serial.printf("{\"tipo\":\"error\",\"donde\":\"spi_slave_initialize\","
                    "\"codigo\":%d}\n", (int)r);
    }
    return;
  }

  esclavoSPIListo = true;

  // Hay que dejar una transaccion encolada ANTES de que el Portenta empiece
  // a meter reloj; si no, la primera lectura saldria vacia.
  encolarTransaccionSPI();
}


/*
 * Se llama desde loop(). No bloquea: si el Portenta todavia no ha leido,
 * sale en la primera comprobacion.
 *
 * El unico momento seguro para tocar bufferSalidaSPI es justo entre dos
 * transacciones, que es lo que hace esta funcion.
 */
void atenderEsclavoSPI() {
  if (!esclavoSPIListo) {
    return;
  }

  if (transaccionSPIEnCurso) {
    spi_slave_transaction_t* hecha = NULL;

    // Espera cero: solo queremos saber si ya termino.
    if (spi_slave_get_trans_result(HOST_SPI_ESCLAVO, &hecha, 0) != ESP_OK) {
      return;
    }

    transaccionSPIEnCurso = false;
    transaccionesSPI++;

    /*
     * Aqui esta el sentido que faltaba. El bloque que el Portenta acaba
     * de mandar ya esta en bufferEntradaSPI, puesto por el DMA durante la
     * misma transaccion en la que el se llevaba nuestra linea: SPI es
     * full duplex y las dos direcciones viajan a la vez.
     *
     * Se trata AHORA, antes de encolar la siguiente transaccion, que es
     * el unico momento en que el DMA no esta escribiendo ese buffer.
     */
    procesarBloqueDelPortenta();
  }

  if (hayLineaPendienteSPI) {
    memset(bufferSalidaSPI, 0, TAM_BLOQUE_SPI);
    strncpy((char*)bufferSalidaSPI, lineaPendienteSPI, TAM_BLOQUE_SPI - 1);
    hayLineaPendienteSPI = false;
  }

  encolarTransaccionSPI();
}


// --- Linea $MET para la aplicacion ---------------------------------------

/*
 * Anexa ",clave:valor" solo si esa magnitud ha llegado alguna vez.
 *
 * Omitirla es mejor que mandar un cero: la aplicacion ignora las claves
 * que no vienen, mientras que un cero lo pintaria como una medida real y
 * no habria forma de distinguirlo despues.
 */
void anexarMet(char* destino, size_t capacidad,
               const char* clave, int magnitud, int decimales) {
  if (!visto[magnitud]) {
    return;
  }

  size_t usado = strlen(destino);

  if (usado + 1 >= capacidad) {
    return;
  }

  snprintf(destino + usado, capacidad - usado,
           ",%s:%.*f", clave, decimales, valor[magnitud]);
}


/*
 * Arma la linea que espera la pantalla meteorologica de la aplicacion:
 *
 *   $MET,T:26.5,H:57.4,W:0.0,G:0.0,D:350,U:0.52,S:84.4,B:8.52,Q:-52.0,R:0.0
 *
 * NO sustituye a la linea JSON. Esa sigue yendo a la Pi tal cual, con UNA
 * medida real por trama.
 *
 * Esta lleva el ultimo valor conocido de cada magnitud, porque cada trama
 * del Davis trae viento y direccion mas UNA sola magnitud rotatoria: la
 * temperatura y la humedad nunca llegan juntas. Sin ese arrastre, la
 * pantalla del movil parpadearia con casi todo vacio.
 *
 * El arrastre aqui es legitimo y es la misma distincion que ya hace la
 * tabla ancha: esto es PRESENTACION, no archivo. Lo que se guarda en la
 * base de datos sigue siendo una medida por trama con su instante real.
 */
void construirLineaMet(char* destino, size_t capacidad) {
  snprintf(destino, capacidad, "$MET");

  anexarMet(destino, capacidad, "T", M_TEMP,   1);
  anexarMet(destino, capacidad, "H", M_HUM,    1);
  anexarMet(destino, capacidad, "W", M_VIENTO, 1);
  anexarMet(destino, capacidad, "G", M_RACHA,  1);
  anexarMet(destino, capacidad, "D", M_DIR,    0);
  anexarMet(destino, capacidad, "U", M_UV,     2);
  anexarMet(destino, capacidad, "S", M_SOLAR,  1);
  anexarMet(destino, capacidad, "B", M_BAT,    2);
  anexarMet(destino, capacidad, "Q", M_RSSI,   1);

  /*
   * La lluvia va aparte: no esta en valor[] porque no es una lectura
   * instantanea sino un acumulado que se construye a partir de las
   * diferencias del contador de vuelcos.
   *
   * ultimaCuenta < 0 significa que todavia no ha llegado ninguna trama
   * del pluviometro, y entonces lluviaMm es un cero que no se ha medido.
   */
  if (ultimaCuenta >= 0) {
    size_t usado = strlen(destino);

    if (usado + 1 < capacidad) {
      snprintf(destino + usado, capacidad - usado, ",R:%.1f", lluviaMm);
    }
  }
}


// --- Lineas que llegan de fuera ------------------------------------------

/*
 * Filtro de lo que aparece en el buffer de entrada del SPI.
 *
 * Hace falta porque el Portenta clockea el bus en CADA envio suyo, tenga
 * algo que decir o no, y porque un cable suelto deja la linea al aire.
 * Sin filtro se colarian bloques vacios y basura en el flujo que va a la
 * Raspberry.
 */
bool esLineaExternaValida(const char* linea) {
  if (linea[0] == '\0') {
    return false;               // bloque vacio: no habia nada que leer
  }

  if ((uint8_t)linea[0] == 0xFF) {
    return false;               // nadie gobierna la linea: esta al aire
  }

  int longitud = (int)strlen(linea);

  if (longitud < LONGITUD_MINIMA_LINEA) {
    return false;
  }

  // Todo lo que manda el Portenta es texto. Un byte no imprimible
  // significa ruido o una transaccion desalineada, no un dato.
  for (int i = 0; i < longitud; i++) {
    if (!isprint((unsigned char)linea[i])) {
      return false;
    }
  }

  return true;
}


/*
 * Copia escapando lo que rompería una cadena JSON.
 *
 * Las lineas $ADC y $RTC no traen comillas ni barras, pero por esta misma
 * via pasan los mensajes manuales que alguien escriba en el monitor del
 * Portenta o mande desde la app, y ahi puede venir cualquier cosa. Un
 * comillas sin escapar dejaria a la Pi con una linea JSON rota.
 */
void escaparParaJson(const char* origen, char* destino, size_t capacidad) {
  size_t j = 0;

  for (size_t i = 0; origen[i] != '\0' && j + 2 < capacidad; i++) {
    char c = origen[i];

    if (c == '"' || c == '\\') {
      destino[j++] = '\\';
      destino[j++] = c;
    } else if ((unsigned char)c < 0x20) {
      // Un caracter de control dentro de una cadena JSON es invalido.
      destino[j++] = ' ';
    } else {
      destino[j++] = c;
    }
  }

  destino[j] = '\0';
}


/*
 * Publica hacia la Raspberry una linea que viene de otro equipo.
 *
 * Se envuelve en JSON en lugar de soltarla cruda porque lo que sale por
 * este puerto ya es un flujo de lineas JSON: los errores y el estado
 * usan el mismo formato. Asi ingesta_davis.py no tiene que aprender una
 * segunda sintaxis, solo un "tipo" nuevo.
 *
 * "origen" identifica de quien viene. Hoy solo lo llama el SPI del
 * Portenta; si algun dia entra la otra Heltec, se llama igual con otro
 * origen y no hay nada mas que tocar.
 */
void publicarLineaExterna(const char* origen, const char* linea) {
  if (SALIDA_TABLA) {
    Serial.printf("  [%s] %s\n", origen, linea);
    return;
  }

  /*
   * Se escribe directo al puerto, sin pasar por lineaJson, por dos
   * motivos:
   *
   *  - lineaJson mide TAM_BLOQUE_SPI (192) porque ese es el tamano del
   *    bloque SPI. Una linea del Portenta puede ocupar ya casi esos 192,
   *    y al meterla dentro de {"tipo":"externo",...} se pasaria: la
   *    cadena saldria cortada a medias y la Pi recibiria JSON invalido.
   *
   *  - lineaJson es ademas la linea del Davis que espera para irse por
   *    SPI. Escribir aqui encima seria pisarla.
   */
  char escapada[TAM_BLOQUE_SPI * 2];

  escaparParaJson(linea, escapada, sizeof(escapada));

  Serial.printf("{\"tipo\":\"externo\",\"origen\":\"%s\",\"linea\":\"%s\"}\n",
                origen, escapada);
}


/*
 * Trata el bloque que el Portenta acaba de clockear.
 *
 * OJO: NO se filtran duplicados aqui, al reves que en el lado del
 * Portenta. Alli hacia falta porque esta placa repite su buffer de
 * salida mientras no tiene linea nueva. Aqui cada transaccion completada
 * es un envio distinto del Portenta, asi que dos lineas $ADC identicas
 * seguidas son dos muestras reales con las entradas quietas, y tirarlas
 * seria perder datos.
 */
void procesarBloqueDelPortenta() {
  // Se saca del buffer del DMA con terminador propio: nada garantiza que
  // lo que venga del bus este bien cerrado, y un strlen() sobre un buffer
  // sin '\0' se saldria del array.
  memcpy(bloqueDelPortenta, bufferEntradaSPI, TAM_BLOQUE_SPI - 1);
  bloqueDelPortenta[TAM_BLOQUE_SPI - 1] = '\0';

  if (!esLineaExternaValida(bloqueDelPortenta)) {
    bloquesDescartados++;
    return;
  }

  lineasDelPortenta++;
  ultimaLineaPortenta = millis();

  publicarLineaExterna("portenta", bloqueDelPortenta);
}


// --- Enlace con la otra Heltec (ESP-NOW) ----------------------------------

#if USAR_ESPNOW

/*
 * Aviso de ESP-NOW. Corre en la tarea de WiFi, NO en loop().
 *
 * Por eso aqui no se imprime nada ni se llama a publicarLineaExterna: solo
 * se copia el mensaje y se levanta la bandera. Todo lo demas lo hace
 * atenderEspNow() desde loop().
 */
void alRecibirEspNow(const esp_now_recv_info_t* info,
                     const uint8_t* datos,
                     int longitud) {
  (void)info;

  if (longitud <= 0) {
    return;
  }

  if (hayMensajeEspNow) {
    // loop() todavia no ha recogido la anterior. Se prefiere perder esta
    // a machacar el buffer mientras se esta leyendo.
    mensajesEspNowPerdidos++;
    return;
  }

  int copiar = (longitud < CAPACIDAD_ESPNOW) ? longitud : CAPACIDAD_ESPNOW;

  memcpy(mensajeEspNow, datos, copiar);
  mensajeEspNow[copiar] = '\0';

  // El puente incluye el salto de linea del Mega dentro del mensaje.
  while (copiar > 0 &&
         (mensajeEspNow[copiar - 1] == '\n' ||
          mensajeEspNow[copiar - 1] == '\r')) {
    copiar--;
    mensajeEspNow[copiar] = '\0';
  }

  hayMensajeEspNow = true;
}


void iniciarEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // El canal tiene que ser el mismo que el del puente; si no, no se oyen.
  esp_wifi_set_channel(CANAL_WIFI, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    espNowListo = false;

    if (!SALIDA_TABLA) {
      Serial.printf("{\"tipo\":\"error\",\"donde\":\"esp_now_init\","
                    "\"codigo\":0}\n");
    }

    return;
  }

  /*
   * Solo se registra la recepcion. Esta placa no contesta nada al puente:
   * los comandos se los sigue mandando heltec_receptor.ino, y meter aqui
   * un segundo emisor solo serviria para pisarse.
   */
  esp_now_register_recv_cb(alRecibirEspNow);

  espNowListo = true;

  if (!SALIDA_TABLA) {
    Serial.printf("{\"tipo\":\"inicio\",\"que\":\"espnow\",\"canal\":%d,"
                  "\"mac\":\"%s\"}\n",
                  CANAL_WIFI, WiFi.macAddress().c_str());
  }
}


/*
 * Recoge desde loop() lo que haya dejado el aviso de ESP-NOW.
 * Se reutiliza el mismo filtro y la misma salida que para el Portenta.
 */
void atenderEspNow() {
  if (!hayMensajeEspNow) {
    return;
  }

  hayMensajeEspNow = false;

  if (!esLineaExternaValida(mensajeEspNow)) {
    bloquesDescartados++;
    return;
  }

  lineasDeOtraHeltec++;
  ultimaLineaOtraHeltec = millis();

  publicarLineaExterna(ORIGEN_OTRA_HELTEC, mensajeEspNow);

  /*
   * Y se le ofrece tambien al Portenta, que es quien se la pasa a la
   * aplicacion por BLE.
   *
   * Solo si no hay ya una linea esperando: la del Davis manda, y no vale
   * la pena pisar una medida meteorologica con una lectura del Mega. Es
   * la misma regla que usa la linea de estado mas abajo.
   *
   * No hace falta repartir mas fino: el Portenta clockea el bus dos o
   * tres veces por segundo y el Davis solo produce una linea cada 2.5 s,
   * asi que sobran huecos para las dos fuentes.
   */
  if (!hayLineaPendienteSPI) {
    prepararLineaSPI(mensajeEspNow);
  }
}

#endif  // USAR_ESPNOW


// --- Pantalla de estado ---------------------------------------------------

#if USAR_OLED

/*
 * Devuelve OK, MUDO o --- segun cuanto lleve callada una fuente.
 * "total" a cero significa que no ha llegado nada desde el arranque, que
 * no es lo mismo que haber dejado de llegar.
 */
const char* estadoFuente(unsigned long ultimo,
                         unsigned long limite,
                         unsigned long total,
                         unsigned long ahora) {
  if (total == 0) {
    return "---";
  }

  if ((ahora - ultimo) > limite) {
    return "MUDO";
  }

  return "OK";
}


void iniciarPantalla() {
  // La OLED se alimenta a traves de Vext. Si no se enciende primero, no
  // responde en el bus y begin() falla sin mas explicacion.
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);      // activo a nivel bajo
  delay(50);

  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(20);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(20);

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.setClock(400000);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, DIRECCION_OLED)) {
    pantallaLista = false;

    if (!SALIDA_TABLA) {
      Serial.printf("{\"tipo\":\"error\",\"donde\":\"oled\",\"codigo\":0}\n");
    }

    return;
  }

  pantallaLista = true;

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.println("HELTEC PUENTE");
  oled.println("iniciando...");
  oled.display();
}


/*
 * Redibuja como mucho una vez por segundo. El refresco entero son unos
 * 25 ms de I2C bloqueante, asi que no conviene hacerlo mas a menudo.
 */
/*
 * Pinta una fuente en dos renglones: estado y antiguedad arriba, cuenta
 * abajo. Con tres fuentes mas titulo y pie salen justo los 8 renglones
 * que caben a tamano 1 en 64 pixeles.
 */
void pintarFuente(const char* nombre,
                  unsigned long ultimo,
                  unsigned long limite,
                  unsigned long total,
                  unsigned long ahora) {
  oled.printf("%-8s %-4s",
              nombre,
              estadoFuente(ultimo, limite, total, ahora));

  if (ultimo == 0) {
    oled.println();
  } else {
    oled.printf(" %lus\n", (ahora - ultimo) / 1000);
  }

  oled.printf("  %lu\n", total);
}


void actualizarPantalla(unsigned long ahora) {
  if (!pantallaLista) {
    return;
  }

  if ((ahora - ultimaPantalla) < PERIODO_PANTALLA_MS) {
    return;
  }

  ultimaPantalla = ahora;

  oled.clearDisplay();
  oled.setCursor(0, 0);

  oled.printf("PUENTE %s\n", sincronizado ? "sync" : "----");

  pintarFuente("DAVIS", ultimaTramaDavis, SILENCIO_DAVIS_MS,
               recibidas, ahora);

  pintarFuente("PORTENTA", ultimaLineaPortenta, SILENCIO_PORTENTA_MS,
               lineasDelPortenta, ahora);

#if USAR_ESPNOW
  pintarFuente("HELTEC2", ultimaLineaOtraHeltec, SILENCIO_HELTEC2_MS,
               lineasDeOtraHeltec, ahora);
#else
  oled.println("HELTEC2  off");
  oled.println();
#endif

  oled.printf("perdidas %lu\n", perdidas);

  oled.display();
}

#endif  // USAR_OLED


void comprobar(const char* paso, int16_t estado) {
  if (estado != RADIOLIB_ERR_NONE) {
    Serial.printf("{\"tipo\":\"error\",\"donde\":\"%s\",\"codigo\":%d}\n",
                  paso, estado);
  }
}


float freqActual() {
  return CANALES_MHZ[SECUENCIA_SALTO[posSecuencia]];
}


void sintonizar() {
  comprobar("standby", radio.standby());
  comprobar("setFrequency", radio.setFrequency(freqActual()));
  comprobar("startReceive", radio.startReceive());
}


unsigned long instanteSalto(unsigned long n) {
  return tRef + (n * MEDIO_MS_POR_SALTO) / 2;
}


void avanzarCanal() {
  posSecuencia = (posSecuencia + 1) % NUM_CANALES;
  sintonizar();
}


void anotar(int magnitud, float v, unsigned long ahora) {
  valor[magnitud] = v;
  instante[magnitud] = ahora;
  visto[magnitud] = true;
}


// Formatea una celda. Las magnitudes que aun no han llegado salen como "--"
// en vez de como un cero, que seria mentira.
void celda(char* destino, size_t n, int magnitud, int decimales) {
  if (!visto[magnitud]) {
    snprintf(destino, n, "--");
  } else {
    snprintf(destino, n, "%.*f", decimales, valor[magnitud]);
  }
}


void imprimirCabecera() {
  Serial.println();
  Serial.printf("  recibidas %lu   perdidas %lu   descartadas %lu   %s\n",
                recibidas, perdidas, descartadas,
                sincronizado ? "enganchado" : "BUSCANDO");
  Serial.println("TIEMPO      TEMP    HUM  VIENTO   RACHA   DIR  LLUVIA     UV   SOLAR    BAT    RSSI  EDAD");
  Serial.println("            degC      %    km/h    km/h   deg      mm           W/m2      V     dBm     s");
  Serial.println("--------   -----  -----  ------  ------  ----  ------  -----  ------  -----  ------  ----");
}


void imprimirFila(unsigned long ahora) {
  unsigned long seg = ahora / 1000;
  char tiempo[12];
  snprintf(tiempo, sizeof(tiempo), "%02lu:%02lu:%02lu",
           seg / 3600, (seg / 60) % 60, seg % 60);

  char sTemp[10], sHum[10], sVien[10], sRach[10], sDir[10];
  char sUv[10], sSol[10], sBat[10], sRssi[10], sLluv[10];

  celda(sTemp, sizeof(sTemp), M_TEMP,   1);
  celda(sHum,  sizeof(sHum),  M_HUM,    1);
  celda(sVien, sizeof(sVien), M_VIENTO, 1);
  celda(sRach, sizeof(sRach), M_RACHA,  1);
  celda(sDir,  sizeof(sDir),  M_DIR,    0);
  celda(sUv,   sizeof(sUv),   M_UV,     2);
  celda(sSol,  sizeof(sSol),  M_SOLAR,  1);
  celda(sBat,  sizeof(sBat),  M_BAT,    2);
  celda(sRssi, sizeof(sRssi), M_RSSI,   1);

  if (ultimaCuenta < 0) {
    snprintf(sLluv, sizeof(sLluv), "--");
  } else {
    snprintf(sLluv, sizeof(sLluv), "%.1f", lluviaMm);
  }

  // La edad de la fila es la de su dato meteorologico mas rancio: si se
  // mantiene baja, es que todas las magnitudes se estan refrescando.
  //
  // La bateria queda fuera del calculo a proposito: su mensaje solo llega
  // cada 150 s aproximadamente, asi que dominaria la columna siempre y la
  // dejaria inservible para lo que interesa. El RSSI tambien, porque llega
  // en cada trama y por tanto nunca esta rancio.
  unsigned long edad = 0;
  for (int i = 0; i < N_MAGNITUDES; i++) {
    if (i == M_BAT || i == M_RSSI) {
      continue;
    }
    if (visto[i]) {
      unsigned long e = (ahora - instante[i]) / 1000;
      if (e > edad) {
        edad = e;
      }
    }
  }

  Serial.printf("%8s   %5s  %5s  %6s  %6s  %4s  %6s  %5s  %6s  %5s  %6s  %4lu\n",
                tiempo, sTemp, sHum, sVien, sRach, sDir,
                sLluv, sUv, sSol, sBat, sRssi, edad);
}


// Actualiza el estado interno con la trama recibida, y en modo JSON la
// publica ademas por el puerto serie.
void procesar(const uint8_t* t, float rssi, unsigned long ahora) {
  int msg = (t[0] >> 4) & 0x0F;
  int bat = (t[0] & 0x08) ? 1 : 0;

  // Viento y direccion viajan en TODAS las tramas, sea cual sea el tipo.
  float vientoKmh = t[1] * 1.609344;
  int dir = (t[2] == 0) ? 360 : (int)(9.0 + t[2] * 342.0 / 255.0 + 0.5);

  anotar(M_VIENTO, vientoKmh, ahora);
  anotar(M_DIR, dir, ahora);
  anotar(M_RSSI, rssi, ahora);

  // La linea se arma en memoria SIEMPRE, tambien en modo tabla: el enlace
  // con el Portenta no debe depender de cual de las dos salidas por serie
  // este activa.
  iniciarLineaJson();
  anexarJson("{\"tipo\":\"dato\",\"t\":%lu,\"freq\":%.3f,\"rssi\":%.1f,"
             "\"msg\":%d,\"bat\":%d,\"viento_kmh\":%.1f,\"dir\":%d",
             ahora, freqActual(), rssi, msg, bat, vientoKmh, dir);

  switch (msg) {
    case 0x2: {   // tension del supercondensador / pila de respaldo
      // La fuente documenta esta formula para la Vantage Vue, pero en esta
      // VP2 Plus da 3.08 V, que es justo lo que cabe esperar de la pila de
      // litio CR-123A. Sirve para vigilar cuando se agota el respaldo.
      float voltios = ((t[3] * 4) + ((t[4] & 0xC0) / 64)) / 100.0;
      anotar(M_BAT, voltios, ahora);
      anexarJson(",\"bateria_v\":%.2f", voltios);
      break;
    }
    case 0x4: {   // indice UV
      float uv = ((((int)t[3] << 8) + t[4]) >> 6) / 50.0;
      anotar(M_UV, uv, ahora);
      anexarJson(",\"uv\":%.2f", uv);
      break;
    }
    case 0x5: {   // tasa de lluvia. 0xFF significa "sin lluvia".
      if (t[3] == 0xFF) {
        anexarJson(",\"lluvia_tasa\":0");
      } else {
        // Sin verificar contra lluvia real: se publica en bruto para
        // calibrarlo el dia que llueva, en vez de inventar la conversion.
        anexarJson(",\"lluvia_tasa_b3\":%d,\"lluvia_tasa_b4\":%d", t[3], t[4]);
      }
      break;
    }
    case 0x6: {   // radiacion solar
      float solar = ((((int)t[3] << 8) + t[4]) >> 6) * 1.757936;
      anotar(M_SOLAR, solar, ahora);
      anexarJson(",\"solar_wm2\":%.1f", solar);
      break;
    }
    case 0x8: {   // temperatura exterior
      float tempF = (t[3] * 256.0 + t[4]) / 160.0;
      float tempC = (tempF - 32.0) * 5.0 / 9.0;
      anotar(M_TEMP, tempC, ahora);
      anexarJson(",\"temp_c\":%.1f", tempC);
      break;
    }
    case 0x9: {   // racha de viento
      float racha = t[3] * 1.609344;
      anotar(M_RACHA, racha, ahora);
      anexarJson(",\"racha_kmh\":%.1f", racha);
      break;
    }
    case 0xA: {   // humedad relativa
      float hum = ((((int)(t[4] >> 4)) << 8) + t[3]) / 10.0;
      anotar(M_HUM, hum, ahora);
      anexarJson(",\"humedad\":%.1f", hum);
      break;
    }
    case 0xE: {   // contador de vuelcos del pluviometro, 7 bits
      int cuenta = t[3] & 0x7F;
      if (ultimaCuenta >= 0) {
        int delta = cuenta - ultimaCuenta;
        if (delta < 0) {
          delta += 128;    // el contador dio la vuelta
        }
        lluviaMm += delta * MM_POR_VUELCO;
      }
      ultimaCuenta = cuenta;
      anexarJson(",\"lluvia_cuenta\":%d,\"mm_por_vuelco\":%.1f",
                 cuenta, MM_POR_VUELCO);
      break;
    }
    default: {
      // Tipos que no decodificamos (0x3 no esta documentado en ninguna
      // fuente que encontrara). Se publican en bruto por si luego
      // interesan, en vez de tirarlos.
      anexarJson(",\"b3\":%d,\"b4\":%d,\"b5\":%d", t[3], t[4], t[5]);
      break;
    }
  }

  anexarJson("}");

  // Salida por el puerto serie: exactamente los mismos bytes que antes,
  // solo que escritos de una vez en lugar de a trozos.
  if (!SALIDA_TABLA) {
    Serial.print(lineaJson);
    Serial.print("\n");
  }

  /*
   * Al Portenta se le ofrece $MET, no el JSON.
   *
   * La aplicacion enruta por el prefijo de la linea y su pantalla
   * meteorologica solo sabe leer "$MET". Antes le llegaba la linea JSON,
   * que no reconoce, y por eso esas graficas estaban en blanco aunque el
   * enlace funcionara.
   *
   * La Pi no se entera de este cambio: sigue recibiendo la JSON entera
   * por el puerto serie, unas lineas mas arriba.
   */
  construirLineaMet(lineaMet, sizeof(lineaMet));

  prepararLineaSPI(lineaMet);
}


void setup() {
  Serial.begin(115200);

  unsigned long inicio = millis();
  while (!Serial && (millis() - inicio) < 3000) {
    delay(10);
  }

  for (int i = 0; i < N_MAGNITUDES; i++) {
    valor[i] = 0.0;
    instante[i] = 0;
    visto[i] = false;
  }

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);

  int estado = radio.beginFSK(CANALES_MHZ[SECUENCIA_SALTO[0]], VELOCIDAD_KBPS,
                              DESVIACION_KHZ, ANCHO_BANDA_KHZ, 10,
                              PREAMBULO_BITS, TCXO_V, USAR_LDO);
  if (estado != RADIOLIB_ERR_NONE) {
    // Sin radio no hay nada que hacer, pero conviene no quedarse mudo: un
    // while(true) en silencio es indistinguible, visto desde la Pi, de un
    // firmware que nunca arranco. Repitiendo el aviso queda constancia en el
    // monitor serie y en el journal del servicio.
    while (true) {
      Serial.printf("{\"tipo\":\"error\",\"donde\":\"beginFSK\",\"codigo\":%d}\n",
                    estado);
      delay(5000);
    }
  }

  uint8_t sync[2] = {SYNC_DAVIS[0], SYNC_DAVIS[1]};
  comprobar("setSyncWord", radio.setSyncWord(sync, 2));
  comprobar("setCRC", radio.setCRC(0));       // lo calculamos nosotros
  comprobar("fixedPacketLength", radio.fixedPacketLengthMode(LONGITUD_TRAMA));
  comprobar("setEncoding", radio.setEncoding(RADIOLIB_ENCODING_NRZ));
  comprobar("setDataShaping", radio.setDataShaping(RADIOLIB_SHAPING_0_5));
  comprobar("setRxBoostedGain", radio.setRxBoostedGainMode(true));

  radio.setPacketReceivedAction(alLlegarTrama);
  comprobar("startReceive", radio.startReceive());

  // Enlace con el Portenta. Va DESPUES del radio para que, si algo fallara
  // aqui, la recepcion del Davis siga funcionando igual: el esclavo SPI es
  // un extra, no una dependencia.
  iniciarEsclavoSPI();

  // Se deja una linea de arranque en el cable desde el primer instante,
  // para que la primera lectura del Portenta ya devuelva algo reconocible
  // en vez de un bloque de ceros.
  iniciarLineaJson();
  anexarJson("{\"tipo\":\"inicio\",\"disp\":\"%s\",\"spi\":%d,\"bloque\":%d}",
             ID_DISPOSITIVO, esclavoSPIListo ? 1 : 0, TAM_BLOQUE_SPI);
  prepararLineaSPI(lineaJson);

  if (SALIDA_TABLA) {
    Serial.println();
    Serial.printf("Davis Vantage Pro2 Plus  --  receptor %s, ID de estacion %d\n",
                  ID_DISPOSITIVO, ID_ESTACION);
    Serial.println("Los huecos se rellenan con el ultimo valor recibido.");
    Serial.println("EDAD = segundos desde la actualizacion mas antigua de la fila.");
    imprimirCabecera();
  } else {
    Serial.printf("{\"tipo\":\"inicio\",\"disp\":\"%s\",\"etapa\":3,"
                  "\"id\":%d,\"canales\":%d,\"periodo_ms\":2562.5}\n",
                  ID_DISPOSITIVO, ID_ESTACION, NUM_CANALES);
  }

#if USAR_ESPNOW
  /*
   * Va DESPUES del radio y del SPI, por el mismo criterio que ellos: si
   * el WiFi fallara, la captura del Davis y el enlace con el Portenta
   * tienen que seguir funcionando igual.
   */
  iniciarEspNow();
#endif

#if USAR_OLED
  // Va la ultima: si la pantalla fallara, todo lo demas ya esta en marcha.
  iniciarPantalla();
#endif

  ultimaFila = millis();
  ultimoEstado = millis();
}


void loop() {
  unsigned long ahora = millis();

  // Atiende el enlace con el Portenta. Es una comprobacion barata: si el
  // maestro no ha leido todavia, sale de inmediato y no roba tiempo a la
  // ventana de recepcion del Davis.
  atenderEsclavoSPI();

#if USAR_ESPNOW
  atenderEspNow();
#endif

#if USAR_OLED
  actualizarPantalla(ahora);
#endif

  if (tramaLista) {
    tramaLista = false;

    uint8_t crudo[LONGITUD_TRAMA];
    int estado = radio.readData(crudo, LONGITUD_TRAMA);
    float rssi = radio.getRSSI();
    radio.startReceive();

    if (estado == RADIOLIB_ERR_NONE) {
      uint8_t trama[LONGITUD_TRAMA];
      for (int i = 0; i < LONGITUD_TRAMA; i++) {
        trama[i] = invertirBits(crudo[i]);
      }

      bool valida = (crc16Davis(trama, LONGITUD_TRAMA) == 0);
      int id = (trama[0] & 0x07) + 1;

      if (valida && id == ID_ESTACION) {
        recibidas++;
        ultimaTramaDavis = ahora;
        fallosSeguidos = 0;

        procesar(trama, rssi, ahora);

        // Cada trama buena vuelve a poner el reloj en hora, asi que la
        // deriva no se acumula por mucho tiempo que pase.
        tRef = ahora;
        saltosDesdeRef = 0;
        sincronizado = true;

        // La estacion ya salto al siguiente canal, asi que nosotros
        // tambien: el proximo paquete llega alli dentro de 2.5625 s.
        avanzarCanal();
      } else {
        descartadas++;
      }
    }
  }

  if (sincronizado) {
    if ((long)(ahora - (instanteSalto(saltosDesdeRef + 1) + MARGEN_MS)) >= 0) {
      perdidas++;
      fallosSeguidos++;
      saltosDesdeRef++;

      if (fallosSeguidos >= FALLOS_PARA_RESYNC) {
        // Nos hemos desfasado. Mejor quedarse quieto y volver a enganchar
        // que seguir saltando a ciegas: la estacion pasa por este canal
        // cada 12.8 s, asi que la espera es corta.
        sincronizado = false;
        fallosSeguidos = 0;
        if (!SALIDA_TABLA) {
          Serial.printf("{\"tipo\":\"resync\",\"t\":%lu,\"freq\":%.3f}\n",
                        ahora, freqActual());
        }
      } else {
        avanzarCanal();
      }
    }
  }

  if (SALIDA_TABLA) {
    if ((ahora - ultimaFila) >= INTERVALO_FILA_MS) {
      if (filasImpresas >= FILAS_POR_CABECERA) {
        imprimirCabecera();
        filasImpresas = 0;
      }
      imprimirFila(ahora);
      filasImpresas++;
      ultimaFila = ahora;
    }
  } else {
    if ((ahora - ultimoEstado) >= INTERVALO_ESTADO_MS) {
      // Se anade spi_lecturas: cuantas veces ha leido el Portenta desde el
      // arranque. Si se queda en 0, el maestro no esta pidiendo nada y el
      // problema esta en el cableado o en el Portenta, no aqui.
      iniciarLineaJson();
      anexarJson("{\"tipo\":\"estado\",\"t\":%lu,\"sync\":%d,\"freq\":%.3f,"
                 "\"recibidas\":%lu,\"perdidas\":%lu,\"descartadas\":%lu,"
                 "\"spi_lecturas\":%lu,\"portenta_lineas\":%lu,"
                 "\"bloques_descartados\":%lu,\"heltec2_lineas\":%lu,"
                 "\"heltec2_perdidos\":%lu}",
                 ahora, sincronizado ? 1 : 0, freqActual(),
                 recibidas, perdidas, descartadas, transaccionesSPI,
                 lineasDelPortenta, bloquesDescartados,
                 lineasDeOtraHeltec, mensajesEspNowPerdidos);

      Serial.print(lineaJson);
      Serial.print("\n");

      /*
       * Esta linea tambien se ofrece por SPI, pero SOLO si no hay ya una
       * trama del Davis esperando a que el Portenta la recoja: no vale la
       * pena pisar una medida real con un mensaje de estado.
       *
       * Sirve para que por el cable circule algo aunque la estacion no
       * este transmitiendo. Sin esto, con el Davis apagado el Portenta
       * leeria ceros y seria imposible distinguir un cable mal puesto de
       * una estacion en silencio.
       */
      if (!hayLineaPendienteSPI) {
        prepararLineaSPI(lineaJson);
      }
      ultimoEstado = ahora;
    }
  }
}
