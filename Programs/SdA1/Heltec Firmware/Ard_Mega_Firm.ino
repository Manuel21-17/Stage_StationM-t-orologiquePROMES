/*
 * ============================================================================
 *  Sda1.ino  --  Arduino Mega 2560 R3
 * ----------------------------------------------------------------------------
 *  Adquisicion de 16 canales analogicos, escalado a magnitud fisica y
 *  publicacion por I2C hacia una Heltec LoRa (que actua de MAESTRO).
 *
 *  Canales:
 *    A0, A1          -> "ten"    Ta-ext-V-4090  temperatura ambiente  [C]
 *    A2              -> "cur"    Si-I-420T      irradiancia solar     [W/m2]
 *    A3              -> "ten"    Ta-ext-V-4090  temperatura, TERMOSTATO [C]
 *    A4 .. A15       -> "crudo"  sin calibrar   cuentas del ADC       [0-1023]
 *
 *  Rectas de calibracion (ver CALIBRACION.md, V = tension en el pin, 0-5 V):
 *    cur   :  G(W/m2) = 375.0 * V - 375.0     (shunt de 250 ohm, 4-20 mA)
 *    ten   :  T(C)    = 26.0  * V - 40.0      (divisor 10V->5V + seguidor)
 *    crudo :  se publica la cuenta del ADC sin tocar, a la espera de saber
 *             que sensor va en cada pin.
 *
 *  Conexionado:
 *    SDA = D20, SCL = D21  ->  convertidor de nivel 5V/3V3  ->  Heltec
 *    DRDY = D7 (salida, ALTO = hay muestra nueva sin recoger)
 *    RELE = D8 (salida)
 *
 *  PROTOCOLO: el que espera heltec_puente.ino (STAGE_Programs\Mega_I2C).
 *    - Esclavo I2C en 0x2A.
 *    - Trama de 160 bytes = 5 lecturas de 32 (LINEA_CAP / TROZO_I2C alla).
 *    - Texto "Lec:A0:###,A1:###,...,A15:###\n" y ceros hasta 160.
 *    - Valores ENTEROS: el maestro los parsea con strtoul(). Ver la nota de
 *      "VALORES NEGATIVOS" mas abajo.
 *    - Escribir el desplazamiento (0, 32, 64, 96, 128) elige el trozo; el 0
 *      ademas congela la instantanea.
 *    - Registro 0xF0 para recibir comandos, terminados en '\n'.
 *
 *  Comandos (por Serial a 115200, o por I2C en el registro 0xF0). Se comparan
 *  sin distinguir mayusculas, igual que hace la Heltec:
 *    comVel:###    periodo de barrido en ms (5 .. 3600000), se recorta
 *    comRel:1      ventilador ON  a mano (desactiva el termostato)
 *    comRel:0      ventilador OFF a mano (desactiva el termostato)
 *    comAuto:1     devuelve el mando al termostato (arranca asi)
 *    comAuto:0     congela el rele donde este
 *    comSerial:1   eco continuo de las lecturas por Serial (por defecto)
 *    comSerial:0   silencia el eco          [comEco: sigue valiendo de alias]
 *    comEst?       vuelca el estado actual por Serial
 *
 *  VALORES NEGATIVOS: solo pueden serlo A0, A1 y A3 (temperatura bajo cero) y
 *  A2 cuando el lazo esta abierto (-999). Los canales crudos van siempre de 0 a
 *  1023. Un negativo viaja como "-12" y el reenvio por LoRa y ESP-NOW es texto
 *  literal, asi que llega bien; pero parsearLec() de heltec_puente usa
 *  strtoul() sobre uint16_t, y en la OLED de la Heltec se vera como un numero
 *  enorme. Se arregla alla cambiando esos dos tipos a int16_t y strtol().
 *  Aqui no hay nada que hacer sin falsear el dato.
 *
 *  Con el eco activado se emite una linea "Lec:..." por cada barrido, de forma
 *  permanente mientras el Mega este alimentado. La linea se envia a trozos,
 *  segun se vacia el bufer de transmision: nunca se espera al UART.
 *
 *  VENTILADOR: rele de D8 gobernado por la temperatura convertida de A3.
 *  Enciende a 32,0 C o mas y apaga por debajo de 31,0 C (1 C de histeresis,
 *  para que el rele no traquetee rondando el umbral).
 *
 *  Sin delay() ni esperas bloqueantes: pensado para correr dias seguidos.
 * ============================================================================
 */

#include <Wire.h>
#include <ctype.h>

// ---------------------------------------------------------------- hardware --
#define PIN_RELE            8
#define PIN_DRDY            7
#define DIR_I2C             0x2A
#define REG_COMANDO         0xF0

// Muchos modulos de rele son de logica invertida. Poner a 0 si el tuyo se
// activa con nivel BAJO.
#define RELE_ACTIVO_ALTO    1

// ------------------------------------------------- termostato del ventilador
// El rele de D8 gobierna el ventilador. En modo automatico lo manda la
// TEMPERATURA YA CONVERTIDA del canal indicado (grados, no cuentas del ADC).
// Ese canal tiene que ser CANAL_TEN en tipoCanal[]: si no, el termostato no
// actua (ver actualizarTermostato). Mover el termostato = cambiar las dos cosas.
#define CANAL_TERMOSTATO    3        // A3
#define TEMP_ENCENDIDO_C   32.0f     // a partir de aqui, ventilador ON

// El ADC resuelve 0,127 C, asi que sin histeresis el rele traquetearia sin
// parar cuando la temperatura ronde el umbral: enciende, se enfria una decima,
// apaga, sube otra decima... Con 1,0 C se apaga al bajar de 31,0 C.
// Poner a 0.0f si de verdad se quiere el corte seco en 32 C.
#define HISTERESIS_C        1.0f

// ------------------------------------------------------------- adquisicion --
#define N_CANALES           16
#define TAM_BLOQUE          32          // TROZO_I2C en heltec_puente
#define N_BLOQUES           5           // N_TROZOS  en heltec_puente
#define TAM_TRAMA           (TAM_BLOQUE * N_BLOQUES)   // LINEA_CAP = 160

// Los mismos limites que INTERVALO_*_MS de heltec_puente. La Heltec recorta
// el valor para si misma pero reenvia el texto original al Mega, asi que aqui
// hay que recortar igual en vez de rechazar, o los dos ritmos se separan.
#define COMVEL_MIN          5UL
#define COMVEL_MAX          3600000UL
#define COMVEL_DEF          100UL       // INTERVALO_DEF_MS de heltec_puente

// Tension de referencia real del ADC. Alimentando por USB puede caer a 4.6 V:
// midela y ajustela aqui, o use una referencia externa de precision
// (LM4040-5.0) con analogReference(EXTERNAL).
const float VREF = 5.000f;

// Valor que se publica cuando la lectura no es valida.
const float VAL_INVALIDO = -999.0f;

// Umbral de lazo vivo del Si-I-420T: por debajo de 0.95 V (3.8 mA) el lazo de
// corriente esta abierto o el sensor no esta alimentado.
const float CUR_V_MINIMA = 0.95f;

// ----------------------------------------------------------------- canales --
// CANAL_CUR   = lazo de corriente 4-20 mA (Si-I-420T, irradiancia)
// CANAL_TEN   = salida en tension 0-10 V   (Ta-ext-V-4090, temperatura)
// CANAL_CRUDO = sin calibrar: se publica la cuenta del ADC (0..1023)
enum TipoCanal : uint8_t { CANAL_CUR, CANAL_TEN, CANAL_CRUDO };

// Un canal por posicion. Cambiar aqui si se recablea un sensor: es lo unico
// que hay que tocar para asignar una calibracion a un pin.
const TipoCanal tipoCanal[N_CANALES] = {
  CANAL_TEN,                                                   // A0
  CANAL_TEN,                                                   // A1
  CANAL_CUR,                                                   // A2
  CANAL_TEN,                                                   // A3 termostato
  CANAL_CRUDO, CANAL_CRUDO, CANAL_CRUDO,                       // A4  A5  A6
  CANAL_CRUDO, CANAL_CRUDO, CANAL_CRUDO, CANAL_CRUDO,          // A7  A8  A9  A10
  CANAL_CRUDO, CANAL_CRUDO, CANAL_CRUDO, CANAL_CRUDO, CANAL_CRUDO  // A11..A15
};

const uint8_t pinCanal[N_CANALES] = {
  A0, A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15
};

float valorCanal[N_CANALES];

// ------------------------------------------------------- estado del sistema --
unsigned long comVel        = COMVEL_DEF;   // periodo de barrido [ms]
unsigned long tUltimoBarrido = 0;
bool          releEncendido = false;
bool          modoAuto     = true;          // termostato al mando
// El termostato no puede escribir por Serial cuando cambia: podria partir en
// dos una trama "Lec:" que este saliendo. Deja el aviso aqui y lo suelta el
// bucle en el mismo hueco en que atiende los comandos.
int8_t        avisoVent    = -1;            // -1 = nada, 0 = apagado, 1 = ON
bool          ecoSerie     = true;          // eco continuo por Serial

// ---------------------------------------------- doble bufer de publicacion --
// El maestro lee la trama en 6 trozos de 32 B. Para que esos 6 trozos
// pertenezcan todos a la misma muestra, escribir el desplazamiento 0 congela
// la instantanea: mientras dura la rafaga el bucle principal no cambia de
// bufer.
static char             bufDatos[2][TAM_TRAMA];
static volatile uint8_t idxPub  = 0;   // bufer ya terminado
static volatile uint8_t idxLect = 0;   // bufer que esta leyendo el maestro
static volatile uint8_t desplaz = 0;   // desplazamiento de la rafaga en curso
static volatile bool    rafagaActiva = false;
static volatile bool    publicacionPendiente = false;
static unsigned long    tInicioRafaga = 0;

#define TIMEOUT_RAFAGA_MS   200UL

// ------------------------------------------------ eco continuo por Serial --
// A 115200 baudios una trama de ~186 caracteres tarda unos 16 ms en salir, y
// el bufer de transmision del AVR es de solo 64 bytes: un Serial.println()
// bloquearia el bucle hasta vaciarlo. En vez de eso la linea se copia aqui y
// se va empujando byte a byte cada pasada, solo mientras haya sitio.
static char     ecoBuf[TAM_TRAMA + 2];
static uint16_t ecoLen = 0;      // bytes utiles en ecoBuf
static uint16_t ecoPos = 0;      // siguiente byte por enviar
static uint16_t ecoPerdidas = 0; // tramas descartadas por UART demasiado lento

static inline bool ecoOcupado()
{
  return ecoPos < ecoLen;
}

static void encolarEco(const char *linea)
{
  if (ecoOcupado()) {            // la anterior todavia esta saliendo
    if (ecoPerdidas < 65535) ecoPerdidas++;
    return;
  }

  uint16_t n = 0;
  while (linea[n] && n < TAM_TRAMA) { ecoBuf[n] = linea[n]; n++; }
  // La trama ya termina en '\n' (lo necesita el maestro I2C); solo se anade
  // si por lo que sea faltara.
  if (n == 0 || ecoBuf[n - 1] != '\n') ecoBuf[n++] = '\n';

  ecoLen = n;
  ecoPos = 0;
}

static void bombearEco()
{
  while (ecoPos < ecoLen && Serial.availableForWrite() > 0) {
    Serial.write((uint8_t)ecoBuf[ecoPos++]);
  }
}

// ------------------------------------------------------- comandos entrantes --
#define TAM_CMD  32
static char    lineaSerie[TAM_CMD];
static uint8_t nSerie = 0;

static volatile char cmdI2C[TAM_CMD];
static volatile bool cmdI2CListo = false;

// ============================================================================
//  Conversion de cuentas del ADC a magnitud fisica
// ============================================================================
static float cuentasATension(int cuentas)
{
  return (float)cuentas * VREF / 1023.0f;
}

static float escalar(uint8_t canal, int cuentas)
{
  // Sin calibrar: la cuenta del ADC tal cual, sin pasar por tension.
  if (tipoCanal[canal] == CANAL_CRUDO) return (float)cuentas;

  float v = cuentasATension(cuentas);

  if (tipoCanal[canal] == CANAL_CUR) {
    // Si-I-420T: 4-20 mA sobre 250 ohm -> 1-5 V para 0..1500 W/m2.
    if (v < CUR_V_MINIMA) return VAL_INVALIDO;   // lazo abierto
    return 375.0f * v - 375.0f;
  }

  // Ta-ext-V-4090: 0-10 V para -40..+90 C, dividido a la mitad.
  return 26.0f * v - 40.0f;
}

// ============================================================================
//  Barrido de los 16 canales
// ============================================================================
static void barrerCanales()
{
  for (uint8_t i = 0; i < N_CANALES; i++) {
    // La primera conversion tras cambiar de canal arrastra carga del canal
    // anterior por el condensador de muestreo del multiplexor: se descarta.
    (void)analogRead(pinCanal[i]);
    int cuentas = analogRead(pinCanal[i]);
    valorCanal[i] = escalar(i, cuentas);
  }
}

// ============================================================================
//  Construccion de la trama ASCII
//    Lec:A0:812,A1:-999,A2:0,A3:23,...,A15:22\n   y ceros hasta 160 bytes
//
//  Enteros, no decimales: heltec_puente lee la trama en un bufer de 160 B y
//  la parsea con strtoul(). En el peor caso ("-999" o "1500" en los 16
//  canales) la linea mide 138 bytes, con 22 de margen. Con dos decimales
//  serian 185 y no cabria.
// ============================================================================
static void construirTrama(char *destino)
{
  memset(destino, 0, TAM_TRAMA);

  uint16_t p = 0;
  const char *cab = "Lec:";
  while (*cab) destino[p++] = *cab++;

  char num[8];
  for (uint8_t i = 0; i < N_CANALES; i++) {
    if (p + 12 >= TAM_TRAMA) break;          // salvaguarda: nunca desbordar

    destino[p++] = 'A';
    if (i >= 10) destino[p++] = '1';
    destino[p++] = (char)('0' + (i % 10));
    destino[p++] = ':';

    long v = lround(valorCanal[i]);
    itoa((int)v, num, 10);
    for (char *q = num; *q; q++) destino[p++] = *q;

    if (i < N_CANALES - 1) destino[p++] = ',';
  }

  // El maestro busca el '\n' para saber donde acaba el texto:
  //   char *fin = strchr(lineaCruda, '\n');
  destino[p++] = '\n';
  destino[p]   = '\0';
}

// ============================================================================
//  Publicacion: cambia el bufer visible para el maestro
// ============================================================================
static void publicarTrama()
{
  uint8_t sreg = SREG;
  cli();
  if (!rafagaActiva) {
    idxPub ^= 1;
    idxLect = idxPub;
    publicacionPendiente = false;
    // heltec_puente lo lee asi:
    //   if (digitalRead(PIN_DRDY) == LOW) -> "el Mega aun no tiene nada nuevo"
    digitalWrite(PIN_DRDY, HIGH);     // ALTO = muestra nueva sin recoger
  } else {
    publicacionPendiente = true;      // se publicara al terminar la rafaga
  }
  SREG = sreg;
}

// ============================================================================
//  Callbacks de I2C
//  Se ejecutan en contexto de interrupcion: cortos, sin coma flotante y sin
//  llamadas a Serial.
// ============================================================================
static void alRecibirI2C(int n)
{
  if (n <= 0) return;

  uint8_t reg = (uint8_t)Wire.read();
  n--;

  if (reg == REG_COMANDO) {
    uint8_t k = 0;
    while (n-- > 0) {
      char c = (char)Wire.read();
      if (k < TAM_CMD - 1 && c != '\n' && c != '\r' && c != '\0') cmdI2C[k++] = c;
    }
    cmdI2C[k] = '\0';
    cmdI2CListo = true;
    return;
  }

  // Desplazamiento de lectura: solo multiplos de 32 dentro de la trama.
  if ((reg % TAM_BLOQUE) == 0 && reg < TAM_TRAMA) {
    desplaz = reg;
    if (reg == 0) {
      idxLect = idxPub;              // congela la instantanea
      rafagaActiva = true;
      digitalWrite(PIN_DRDY, LOW);   // el maestro ya la esta recogiendo
    }
  }

  while (n-- > 0) Wire.read();       // descarta sobrantes
}

static void alPedirI2C()
{
  Wire.write((const uint8_t *)&bufDatos[idxLect][desplaz], TAM_BLOQUE);

  desplaz += TAM_BLOQUE;
  if (desplaz >= TAM_TRAMA) {
    desplaz = 0;
    rafagaActiva = false;            // rafaga completa
  }
}

// ============================================================================
//  Rele
// ============================================================================
static void aplicarRele()
{
#if RELE_ACTIVO_ALTO
  digitalWrite(PIN_RELE, releEncendido ? HIGH : LOW);
#else
  digitalWrite(PIN_RELE, releEncendido ? LOW : HIGH);
#endif
}

// ============================================================================
//  Termostato del ventilador
//
//  Compara la MAGNITUD CONVERTIDA del canal, no la cuenta del ADC:
//  valorCanal[] ya sale de escalar(), asi que TEMP_ENCENDIDO_C son grados.
//  Se llama despues de cada barrido.
// ============================================================================
static void actualizarTermostato()
{
  if (!modoAuto) return;

  // Salvaguarda: si alguien pone ese canal como CANAL_CRUDO o CANAL_CUR, el
  // valor deja de ser una temperatura y comparar contra 32 no significa nada.
  // Mejor no tocar el rele que encenderlo por un numero equivocado.
  if (tipoCanal[CANAL_TERMOSTATO] != CANAL_TEN) return;

  float t = valorCanal[CANAL_TERMOSTATO];
  bool  nuevo = releEncendido;

  if (t >= TEMP_ENCENDIDO_C) {
    nuevo = true;
  } else if (t < (TEMP_ENCENDIDO_C - HISTERESIS_C)) {
    nuevo = false;
  }
  // Entre los dos umbrales se mantiene como estaba: eso es la histeresis.

  if (nuevo != releEncendido) {
    releEncendido = nuevo;
    aplicarRele();
    avisoVent = nuevo ? 1 : 0;
  }
}

// ============================================================================
//  Interprete de comandos (comun a Serial y a I2C)
// ============================================================================
static void volcarEstado()
{
  Serial.print(F("Est:comVel="));  Serial.print(comVel);
  Serial.print(F("ms,rele="));     Serial.print(releEncendido ? 1 : 0);
  Serial.print(F(",modo="));       Serial.print(modoAuto ? F("auto") : F("manual"));
  Serial.print(F(",umbral="));     Serial.print(TEMP_ENCENDIDO_C, 1);
  Serial.print(F("/"));            Serial.print(TEMP_ENCENDIDO_C - HISTERESIS_C, 1);
  Serial.print(F("C,tA"));         Serial.print(CANAL_TERMOSTATO);
  Serial.print(F("="));            Serial.print(valorCanal[CANAL_TERMOSTATO], 1);
  Serial.print(F(",comSerial="));  Serial.print(ecoSerie ? 1 : 0);
  Serial.print(F(",perdidas="));   Serial.print(ecoPerdidas);
  Serial.print(F(",dirI2C=0x"));   Serial.print(DIR_I2C, HEX);
  Serial.print(F(",trama="));      Serial.print(TAM_TRAMA);
  Serial.println(F("B"));
  Serial.print(bufDatos[idxPub]);      // la trama ya trae su propio '\n'
}

/* Compara sin distinguir mayusculas y devuelve el argumento, igual que
   coincide() en heltec_puente. Hace falta porque la Heltec reenvia el texto
   tal cual lo escribio el usuario: "comvel:100" tiene que valer igual que
   "comVel:100". */
static const char *coincide(const char *linea, const char *clave)
{
  while (*clave) {
    if (tolower((unsigned char)*linea) != tolower((unsigned char)*clave)) return NULL;
    linea++; clave++;
  }
  return linea;
}

static bool leerBooleano(const char *valor, bool &destino)
{
  while (*valor == ' ') valor++;
  if (*valor == '1') { destino = true;  return true; }
  if (*valor == '0') { destino = false; return true; }
  return false;
}

static void procesarComando(const char *cmd)
{
  while (*cmd == ' ' || *cmd == '\t') cmd++;
  if (*cmd == '\0') return;

  const char *v;

  if ((v = coincide(cmd, "comvel:")) != NULL) {
    unsigned long ms = strtoul(v, NULL, 10);
    if (ms == 0) {
      Serial.println(F("Err:comVel valor invalido"));
      return;
    }
    // Se recorta, no se rechaza: la Heltec ya se recorto a si misma pero
    // reenvia el texto original, y los dos ritmos deben acabar iguales.
    if (ms < COMVEL_MIN) ms = COMVEL_MIN;
    if (ms > COMVEL_MAX) ms = COMVEL_MAX;

    comVel = ms;
    tUltimoBarrido = millis();          // el nuevo ritmo empieza ya
    ecoPerdidas = 0;
    Serial.print(F("Ok:comVel=")); Serial.print(comVel); Serial.println(F("ms"));
    // A 115200 baudios una trama de 138 B tarda ~12 ms en salir por el UART.
    if (ecoSerie && comVel < 15) {
      Serial.println(F("Avi:a este ritmo el eco perdera tramas (usar comSerial:0)"));
    }
    return;
  }

  if ((v = coincide(cmd, "comrel:")) != NULL) {
    bool estado;
    if (!leerBooleano(v, estado)) {
      Serial.println(F("Err:comRel espera 0 o 1"));
      return;
    }
    // Mandar el rele a mano desconecta el termostato: si no, el siguiente
    // barrido lo devolveria a su sitio y pareceria que el comando se ignora.
    modoAuto      = false;
    releEncendido = estado;
    aplicarRele();
    avisoVent     = -1;
    Serial.print(F("Ok:comRel=")); Serial.print(releEncendido ? 1 : 0);
    Serial.println(F(" (modo manual, comAuto:1 devuelve el termostato)"));
    return;
  }

  if ((v = coincide(cmd, "comauto:")) != NULL) {
    bool estado;
    if (!leerBooleano(v, estado)) {
      Serial.println(F("Err:comAuto espera 0 o 1"));
      return;
    }
    modoAuto = estado;
    Serial.print(F("Ok:comAuto=")); Serial.println(modoAuto ? 1 : 0);
    if (modoAuto) actualizarTermostato();   // aplica el umbral ya mismo
    return;
  }

  // comSerial: es el nombre que reenvia la Heltec. comEco: queda como alias.
  if ((v = coincide(cmd, "comserial:")) == NULL) v = coincide(cmd, "comeco:");
  if (v != NULL) {
    bool estado;
    if (!leerBooleano(v, estado)) {
      Serial.println(F("Err:comSerial espera 0 o 1"));
      return;
    }
    ecoSerie = estado;
    if (!ecoSerie) ecoPerdidas = 0;
    Serial.print(F("Ok:comSerial=")); Serial.println(ecoSerie ? 1 : 0);
    return;
  }

  if (coincide(cmd, "comest") != NULL) {
    volcarEstado();
    return;
  }

  Serial.print(F("Err:comando desconocido '"));
  Serial.print(cmd);
  Serial.println(F("'. Validos: comVel:### comRel:0|1 comAuto:0|1 comSerial:0|1 comEst?"));
}

// ---------------------------------------------------- lectura no bloqueante --
static void atenderSerie()
{
  while (Serial.available() > 0) {
    char c = (char)Serial.read();

    if (c == '\n' || c == '\r') {
      if (nSerie > 0) {
        lineaSerie[nSerie] = '\0';
        procesarComando(lineaSerie);
        nSerie = 0;
      }
    } else if (nSerie < TAM_CMD - 1) {
      lineaSerie[nSerie++] = c;
    } else {
      nSerie = 0;                       // linea demasiado larga: se descarta
      Serial.println(F("Err:comando demasiado largo"));
    }
  }
}

static void atenderComandoI2C()
{
  if (!cmdI2CListo) return;

  char copia[TAM_CMD];
  uint8_t sreg = SREG;
  cli();
  memcpy(copia, (const void *)cmdI2C, TAM_CMD);
  cmdI2CListo = false;
  SREG = sreg;

  copia[TAM_CMD - 1] = '\0';
  Serial.print(F("I2C<")); Serial.println(copia);
  procesarComando(copia);
}

// ============================================================================
//  setup / loop
// ============================================================================
void setup()
{
  // Rele en reposo ANTES de configurar el pin como salida, para que no de un
  // pulso espurio al arrancar.
  releEncendido = false;
  aplicarRele();
  pinMode(PIN_RELE, OUTPUT);
  aplicarRele();

  pinMode(PIN_DRDY, OUTPUT);
  digitalWrite(PIN_DRDY, LOW);         // BAJO = sin dato nuevo

  Serial.begin(115200);

  analogReference(DEFAULT);            // AVcc = 5 V

  for (uint8_t i = 0; i < N_CANALES; i++) valorCanal[i] = VAL_INVALIDO;
  barrerCanales();
  actualizarTermostato();              // el ventilador arranca ya en su sitio
  avisoVent = -1;                      // sin avisar: es el estado inicial
  construirTrama(bufDatos[0]);
  construirTrama(bufDatos[1]);

  Wire.begin(DIR_I2C);                 // esclavo
  // Wire.begin() activa los pull-up internos del ATmega, que tiran a 5 V y
  // pelean con el convertidor de nivel. Se desactivan.
  digitalWrite(SDA, LOW);
  digitalWrite(SCL, LOW);
  Wire.onReceive(alRecibirI2C);
  Wire.onRequest(alPedirI2C);

  tUltimoBarrido = millis();
  tInicioRafaga  = tUltimoBarrido;

  Serial.println(F("Sda1 listo. Comandos: comVel:### comRel:0|1 comAuto:0|1 comSerial:0|1 comEst?"));
  if (tipoCanal[CANAL_TERMOSTATO] != CANAL_TEN) {
    Serial.print(F("Avi:A")); Serial.print(CANAL_TERMOSTATO);
    Serial.println(F(" no es CANAL_TEN: el termostato NO va a actuar"));
  }
  volcarEstado();
}

void loop()
{
  unsigned long ahora = millis();

  // --- Eco: empuja lo que quepa en el bufer del UART, sin esperar ---
  bombearEco();

  // --- Comandos ---
  // Se atienden solo entre lineas de eco, para que una respuesta "Ok:" no se
  // cuele en medio de una trama "Lec:". El retraso maximo es el tiempo de una
  // linea (~16 ms a 115200).
  if (!ecoOcupado()) {
    // El aviso del termostato sale antes que nada, en el mismo hueco entre
    // lineas de eco.
    if (avisoVent >= 0) {
      Serial.print(F("Ok:vent="));
      Serial.print(avisoVent);
      Serial.print(F(" (A")); Serial.print(CANAL_TERMOSTATO);
      Serial.print(F("=")); Serial.print(valorCanal[CANAL_TERMOSTATO], 1);
      Serial.println(F("C)"));
      avisoVent = -1;
    }
    atenderSerie();
    atenderComandoI2C();
  }

  // --- Guardian: un maestro que abandone una rafaga a medias no puede
  //     congelar los datos para siempre ---
  if (rafagaActiva) {
    if ((ahora - tInicioRafaga) > TIMEOUT_RAFAGA_MS) {
      uint8_t sreg = SREG;
      cli();
      rafagaActiva = false;
      desplaz = 0;
      SREG = sreg;
      tInicioRafaga = ahora;
    }
  } else {
    tInicioRafaga = ahora;
  }

  // --- Barrido periodico ---
  if ((ahora - tUltimoBarrido) >= comVel) {
    tUltimoBarrido = ahora;

    char *trama = bufDatos[idxPub ^ 1];     // se escribe el bufer inactivo
    barrerCanales();
    actualizarTermostato();                 // con la temperatura recien leida
    construirTrama(trama);
    publicarTrama();

    // El eco sale de la trama recien construida, no de idxPub: si hay una
    // rafaga I2C en curso la publicacion queda pendiente, pero el Serial no
    // tiene por que esperarla.
    if (ecoSerie) encolarEco(trama);
  }

  // --- Publicacion que quedo pendiente por una rafaga I2C ---
  if (publicacionPendiente && !rafagaActiva) {
    publicarTrama();
  }
}
