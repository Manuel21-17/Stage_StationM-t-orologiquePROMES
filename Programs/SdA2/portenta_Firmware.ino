#include <ArduinoBLE.h>
#include <SPI.h>
#include <Wire.h>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cstdio>

// =====================================================
// CONFIGURACIÓN GENERAL
// =====================================================

// Ya no es constexpr: el comando "comvel:#" (Serial o BLE)
// permite modificarlo en tiempo de ejecución.
unsigned long periodoADCMs = 500;

// Límites de seguridad para el comando comvel.
constexpr unsigned long PERIODO_ADC_MS_MIN = 10;
constexpr unsigned long PERIODO_ADC_MS_MAX = 3600000UL; // 1 hora

// Prefijo del comando que cambia el periodo de lectura ADC.
// Formato esperado: comvel:<milisegundos>
const char* PREFIJO_COMANDO_VELOCIDAD = "comvel:";

// Resolución ADC:
// 12 bits -> valores entre 0 y 4095.
constexpr int RESOLUCION_ADC_BITS = 12;

// Entradas analógicas disponibles.
const pin_size_t PINES_ADC[] = {
  A0, A1, A2, A3,
  A4, A5, A6, A7
};

constexpr size_t NUM_CANALES_ADC =
  sizeof(PINES_ADC) / sizeof(PINES_ADC[0]);


// =====================================================
// CONFIGURACIÓN BLE
// =====================================================

const char* NOMBRE_BLE = "PortentaH7";

const char* UUID_SERVICIO =
  "19B10000-E8F2-537E-4F6C-D104768A1214";

const char* UUID_TX =
  "19B10001-E8F2-537E-4F6C-D104768A1214";

const char* UUID_RX =
  "19B10002-E8F2-537E-4F6C-D104768A1214";

BLEService servicioDatos(UUID_SERVICIO);

/*
 * BLERead:
 * La aplicación puede leer el último fragmento.
 *
 * BLENotify:
 * La aplicación recibe una notificación cada vez
 * que el Portenta actualiza la característica.
 *
 * Se utilizan fragmentos de 20 bytes.
 */
BLECharacteristic caracteristicaTX(
  UUID_TX,
  BLERead | BLENotify,
  20
);

/*
 * BLEWrite:
 * La aplicación puede escribir en esta característica
 * para enviar comandos al Portenta (por ejemplo, comvel:#).
 *
 * Se utilizan fragmentos de hasta 20 bytes, igual que TX.
 * Si el comando no cabe en un solo fragmento, la app puede
 * escribir varias veces seguidas: los datos se acumulan
 * hasta encontrar '\n'.
 */
BLECharacteristic caracteristicaRX(
  UUID_RX,
  BLEWrite,
  20
);


// =====================================================
// CONFIGURACIÓN SPI
// =====================================================

/*
 * Pines laterales del Portenta:
 *
 * D7  = CS
 * D8  = COPI / MOSI
 * D9  = CK / SCK
 * D10 = CIPO / MISO
 *
 * El objeto SPI usa automáticamente D8, D9 y D10.
 * Nosotros controlamos manualmente D7.
 */
constexpr uint8_t PIN_CS_HELTEC = 7;

/*
 * Tiene que coincidir EXACTAMENTE con TAM_BLOQUE_SPI de las Heltec.
 *
 * Son 192 y no 128 porque la Heltec del Davis devuelve por MISO una
 * línea JSON del pluviómetro de 144 bytes: con bloques de 128 llegaría
 * cortada y habría que tirarla.
 */
constexpr size_t TAMANO_PAQUETE_SPI = 192;

/*
 * Por debajo de esto lo recibido no puede ser una línea de datos.
 * Sirve para descartar ruido de un byte o dos.
 */
constexpr size_t LONGITUD_MINIMA_LINEA_SPI = 4;

// Frecuencia inicial conservadora.
constexpr uint32_t FRECUENCIA_SPI_HZ = 1000000;

// Heltec configurado como SPI mode 0.
SPISettings configuracionSPI(
  FRECUENCIA_SPI_HZ,
  MSBFIRST,
  SPI_MODE0
);


// =====================================================
// CONFIGURACIÓN RTC DS3231 (I2C)
// =====================================================

/*
 * Módulo DS3231 + AT24C32 (las placas azules tipo ZS-042).
 *
 * Cableado al Portenta H7:
 *
 *     Módulo   Portenta H7          Señal
 *     ------   ------------------   ------------------
 *     VCC      3V3                  ¡NO conectar a 5 V!
 *     GND      GND                  masa común
 *     SDA      D11 (PH_8)           datos I2C
 *     SCL      D12 (PH_7)           reloj I2C
 *
 * D11 y D12 son los pines que el core del Portenta asocia al objeto
 * "Wire", así que Wire.begin() ya deja el bus en esos pines.
 *
 * *** OJO CON LA ALIMENTACIÓN ***
 *
 * La placa del módulo trae dos resistencias de pull-up de 4,7 k
 * conectadas a VCC. Si se alimenta el módulo a 5 V, esas resistencias
 * suben SDA y SCL a 5 V, y los pines del Portenta son de 3,3 V y NO
 * toleran 5 V. Alimentando el módulo desde 3V3 el problema desaparece
 * sin tener que tocar la placa.
 *
 * *** SI SE MONTA UNA PILA CR2032 ***
 *
 * Estas placas llevan un circuito de carga (diodo mas resistencia
 * desde VCC hacia la pila) pensado para una LIR2032 recargable. A
 * 3,3 V apenas circula corriente, pero si se monta una CR2032, que no
 * es recargable, lo limpio es quitar esa resistencia.
 *
 * *** DIRECCIONES I2C DE ESTA PLACA ***
 *
 *   0x68 -> DS3231, el reloj. Es lo único que se usa aquí.
 *   0x57 -> EEPROM AT24C32. Va en el mismo módulo y de momento no se
 *           toca, pero conviene recordar que está ahí antes de colgar
 *           mas cosas del bus.
 */

constexpr uint8_t DIRECCION_I2C_DS3231 = 0x68;

// Registros del DS3231 que se utilizan.
constexpr uint8_t REG_DS3231_SEGUNDOS = 0x00;
constexpr uint8_t REG_DS3231_ESTADO   = 0x0F;
constexpr uint8_t REG_DS3231_TEMP_MSB = 0x11;

// Los siete registros de fecha y hora son consecutivos desde 0x00.
constexpr uint8_t NUM_REGISTROS_FECHA_HORA = 7;

/*
 * Bit 7 del registro de estado: "Oscillator Stop Flag".
 *
 * El DS3231 lo pone a 1 cuando el oscilador se ha detenido, es decir,
 * cuando se ha quedado sin alimentación y sin pila. Mientras valga 1,
 * la fecha y la hora que devuelve NO son de fiar.
 */
constexpr uint8_t MASCARA_OSF = 0x80;

// Cada cuánto se publica la hora por el puerto serie.
unsigned long periodoRTCMs = 1000;

// Comando que pone en hora el reloj: comhora:AAAA-MM-DD hh:mm:ss
const char* PREFIJO_COMANDO_HORA = "comhora:";

// El DS3231 guarda el año con dos dígitos mas un bit de siglo.
// Aquí se trabaja siempre dentro del siglo 21.
constexpr int ANIO_MINIMO_RTC = 2000;
constexpr int ANIO_MAXIMO_RTC = 2099;

/*
 * Fecha y hora ya convertidas de BCD a binario, que es como se
 * manejan dentro del programa.
 */
struct FechaHora
{
  uint16_t anio;
  uint8_t  mes;
  uint8_t  dia;
  uint8_t  diaSemana;   // 1 = domingo ... 7 = sabado
  uint8_t  hora;        // siempre en formato de 24 horas
  uint8_t  minuto;
  uint8_t  segundo;
};


// =====================================================
// BUFFERS
// =====================================================

/*
 * El Heltec puede recibir hasta 128 bytes.
 * Se deja un byte adicional para el terminador '\0'
 * durante la construcción local.
 */
char paqueteADC[TAMANO_PAQUETE_SPI];

// Buffer para lo escrito por el usuario en el Monitor Serie.
char entradaSerial[TAMANO_PAQUETE_SPI];

size_t posicionEntradaSerial = 0;

// Buffer para lo escrito por la app en la característica RX (BLE).
char entradaBLE[TAMANO_PAQUETE_SPI];

size_t posicionEntradaBLE = 0;

/*
 * Línea completa recibida por BLE y todavía sin tratar.
 *
 * El manejador de escritura se ejecuta desde dentro de BLE.poll(), así
 * que allí no se procesa nada: solo se deja la línea aquí y es loop()
 * quien la atiende. De lo contrario habría que volver a entrar en
 * poll() desde dentro de poll() para poder contestar.
 */
char mensajeBLEPendiente[TAMANO_PAQUETE_SPI];

volatile bool hayMensajeBLEPendiente = false;

/*
 * Muchas apps de terminal BLE no añaden '\n' al final de lo que
 * envían. Si hay datos a medias y no llega nada más durante este
 * tiempo, se dan por terminados.
 */
constexpr unsigned long TIEMPO_LIMITE_LINEA_BLE_MS = 250;

/*
 * Lo que la Heltec devuelve por MISO durante el mismo intercambio en
 * que el Portenta le envía su bloque.
 */
char recepcionSPI[TAMANO_PAQUETE_SPI];

/*
 * Última línea válida recibida.
 *
 * La Heltec solo rellena su buffer de salida cuando tiene una línea
 * NUEVA (su atenderEsclavoSPI hace el memcpy bajo un
 * "if (hayLineaPendienteSPI)"). Mientras no la tenga, repite el mismo
 * bloque en cada intercambio, así que sin comparar contra esto la
 * misma línea saldría por el puerto serie una y otra vez.
 */
char ultimaLineaSPI[TAMANO_PAQUETE_SPI];


// =====================================================
// VARIABLES DE ESTADO
// =====================================================

unsigned long tiempoAnteriorADC = 0;
bool estadoBLEAnterior = false;

// Momento de la última publicación de la hora.
unsigned long tiempoAnteriorRTC = 0;

// Momento del último fragmento recibido por BLE, para el corte por
// tiempo cuando la app no manda '\n'.
unsigned long tiempoUltimoFragmentoBLE = 0;

// El DS3231 contestó la última vez que se le habló.
bool rtcPresente = false;

/*
 * Estos dos avisos se imprimen una sola vez cada vez que aparece el
 * problema, no en cada ciclo: la hora sale una vez por segundo y
 * repetir el mensaje llenaría el Monitor Serie.
 */
bool avisoErrorRTCMostrado = false;
bool avisoHoraNoValidaMostrado = false;

// Cuántas líneas se han recibido de la Heltec desde el arranque.
unsigned long lineasRecibidasSPI = 0;

/*
 * Se avisa una sola vez, al llegar la primera línea: es la
 * confirmación de que el enlace con la Heltec funciona de verdad, y
 * repetirlo en cada línea sería ruido.
 */
bool enlaceSPIConfirmado = false;


// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);

  // Espera como máximo tres segundos por el Monitor Serie.
  unsigned long inicioEspera = millis();

  while (!Serial && millis() - inicioEspera < 3000)
  {
    // Espera limitada.
  }

  Serial.println();
  Serial.println("====================================");
  Serial.println("Portenta H7 - ADC / BLE / SPI");
  Serial.println("====================================");

  configurarADC();
  iniciarSPI();
  iniciarRTC();
  iniciarBLE();

  Serial.println();
  Serial.println("Sistema listo.");
  Serial.print("Periodo ADC: ");
  Serial.print(periodoADCMs);
  Serial.println(" ms");
  Serial.print("Periodo RTC: ");
  Serial.print(periodoRTCMs);
  Serial.println(" ms");
  Serial.println("Formato ADC: $ADC,A0:#,...,A7:#");
  Serial.println(
    "Formato RTC: $RTC,FECHA:AAAA-MM-DD,HORA:hh:mm:ss,DIA:#,TEMP:#.##"
  );
  Serial.println();
  Serial.println("Comandos (Serial o BLE):");
  Serial.println(
    "  comvel:<ms>                  periodo de lectura del ADC"
  );
  Serial.println(
    "  comhora:AAAA-MM-DD hh:mm:ss  pone en hora el DS3231"
  );
  Serial.println();
}


// =====================================================
// LOOP
// =====================================================

void loop()
{
  // Mantiene activa la pila BLE.
  BLE.poll();

  actualizarEstadoBLE();

  // Conserva la función del primer programa:
  // enviar por BLE/SPI lo escrito en el Monitor Serie.
  procesarEntradaSerial();

  // Cierra por tiempo una línea BLE a medias, para las apps que no
  // terminan lo que envían con '\n'.
  revisarTiempoLimiteLineaBLE();

  // Atiende, ya fuera de BLE.poll(), lo que haya llegado por BLE.
  if (hayMensajeBLEPendiente)
  {
    hayMensajeBLEPendiente = false;

    procesarMensajeEntrante(mensajeBLEPendiente, "BLE");
  }

  unsigned long tiempoActual = millis();

  if (tiempoActual - tiempoAnteriorADC >= periodoADCMs)
  {
    tiempoAnteriorADC = tiempoActual;

    construirPaqueteADC(
      paqueteADC,
      sizeof(paqueteADC)
    );

    distribuirPaquete(paqueteADC);
  }

  if (tiempoActual - tiempoAnteriorRTC >= periodoRTCMs)
  {
    tiempoAnteriorRTC = tiempoActual;

    publicarFechaHora();
  }
}


// =====================================================
// CONFIGURACIÓN ADC
// =====================================================

void configurarADC()
{
  analogReadResolution(RESOLUCION_ADC_BITS);

  for (size_t i = 0; i < NUM_CANALES_ADC; i++)
  {
    pinMode(PINES_ADC[i], INPUT);
  }

  Serial.print("ADC configurado a ");
  Serial.print(RESOLUCION_ADC_BITS);
  Serial.println(" bits.");
}


// =====================================================
// CONFIGURACIÓN SPI
// =====================================================

void iniciarSPI()
{
  pinMode(PIN_CS_HELTEC, OUTPUT);

  // CS debe permanecer inactivo en HIGH.
  digitalWrite(PIN_CS_HELTEC, HIGH);

  SPI.begin();

  // Ninguna línea recibida todavía.
  ultimaLineaSPI[0] = '\0';

  Serial.println("SPI maestro iniciado (bidireccional).");
  Serial.println("CS: D7");
  Serial.println("MOSI/COPI: D8");
  Serial.println("SCK: D9");
  Serial.println("MISO/CIPO: D10");
  Serial.println("Modo SPI: 0");
  Serial.println("Frecuencia SPI: 1 MHz");
  Serial.print("Tamano de bloque: ");
  Serial.print(TAMANO_PAQUETE_SPI);
  Serial.println(" bytes");
  Serial.println(
    "Lo que devuelva la Heltec se reenvia al puerto serie."
  );
}


// =====================================================
// CONFIGURACIÓN BLE
// =====================================================

void iniciarBLE()
{
  Serial.println("Iniciando Bluetooth Low Energy...");

  if (!BLE.begin())
  {
    Serial.println("ERROR: no fue posible iniciar BLE.");

    while (true)
    {
      // Error crítico.
    }
  }

  BLE.setLocalName(NOMBRE_BLE);
  BLE.setDeviceName(NOMBRE_BLE);

  BLE.setAdvertisedService(servicioDatos);

  servicioDatos.addCharacteristic(caracteristicaTX);
  servicioDatos.addCharacteristic(caracteristicaRX);
  BLE.addService(servicioDatos);

  // Se invoca cada vez que la app escribe en la característica RX.
  caracteristicaRX.setEventHandler(BLEWritten, alRecibirEscrituraBLE);

  const char mensajeInicial[] = "$SYS,READY\n";

  caracteristicaTX.writeValue(
    reinterpret_cast<const uint8_t*>(mensajeInicial),
    strlen(mensajeInicial)
  );

  BLE.advertise();

  Serial.println("BLE iniciado correctamente.");
  Serial.print("Nombre BLE: ");
  Serial.println(NOMBRE_BLE);
}


// =====================================================
// CONSTRUCCIÓN DEL PAQUETE ADC
// =====================================================

void construirPaqueteADC(
  char* destino,
  size_t capacidad
)
{
  int lecturas[NUM_CANALES_ADC];

  for (size_t i = 0; i < NUM_CANALES_ADC; i++)
  {
    lecturas[i] = analogRead(PINES_ADC[i]);
  }

  snprintf(
    destino,
    capacidad,
    "$ADC,"
    "A0:%d,"
    "A1:%d,"
    "A2:%d,"
    "A3:%d,"
    "A4:%d,"
    "A5:%d,"
    "A6:%d,"
    "A7:%d",
    lecturas[0],
    lecturas[1],
    lecturas[2],
    lecturas[3],
    lecturas[4],
    lecturas[5],
    lecturas[6],
    lecturas[7]
  );
}


// =====================================================
// DISTRIBUCIÓN DE PAQUETES
// =====================================================

void distribuirPaquete(const char* paquete)
{
  // Siempre se muestra por el puerto USB.
  Serial.println(paquete);

  // Siempre se comparte con el Heltec.
  enviarPorSPI(paquete);

  // Solo se envía por BLE cuando hay conexión.
  if (BLE.connected())
  {
    enviarPorBLE(paquete);
  }
}


// =====================================================
// ENVÍO BLE
// =====================================================

void enviarPorBLE(const char* texto)
{
  const size_t TAMANO_FRAGMENTO_BLE = 20;

  size_t longitudTexto = strlen(texto);

  /*
   * Se incluye '\n' para indicar a Android
   * dónde termina el paquete.
   */
  size_t longitudTotal = longitudTexto + 1;
  size_t posicion = 0;

  while (posicion < longitudTotal)
  {
    uint8_t fragmento[TAMANO_FRAGMENTO_BLE];

    size_t bytesRestantes = longitudTotal - posicion;

    size_t cantidad =
      bytesRestantes < TAMANO_FRAGMENTO_BLE
        ? bytesRestantes
        : TAMANO_FRAGMENTO_BLE;

    for (size_t i = 0; i < cantidad; i++)
    {
      size_t indiceTexto = posicion + i;

      if (indiceTexto < longitudTexto)
      {
        fragmento[i] =
          static_cast<uint8_t>(texto[indiceTexto]);
      }
      else
      {
        fragmento[i] = '\n';
      }
    }

    caracteristicaTX.writeValue(
      fragmento,
      cantidad
    );

    posicion += cantidad;

    BLE.poll();

    // Separación breve entre notificaciones.
    delay(5);
  }
}


// =====================================================
// ENVÍO SPI
// =====================================================

/*
 * Intercambio con la Heltec.
 *
 * Se llama "enviar" por lo que hace desde fuera, pero en realidad es
 * un intercambio: SPI es full duplex, y en el mismo pulso de reloj que
 * saca un bit por MOSI entra otro por MISO. El byte que devuelve
 * SPI.transfer() es el que la Heltec tenía preparado en su buffer de
 * salida.
 *
 * Antes ese valor se descartaba. Ahora se guarda, y con eso queda
 * resuelta toda la recepción: no hace falta un segundo mecanismo ni
 * pedirle nada a la Heltec.
 */
void enviarPorSPI(const char* texto)
{
  /*
   * La Heltec prepara una transacción de exactamente
   * TAMANO_PAQUETE_SPI bytes, así que el Portenta transmite siempre el
   * bloque completo aunque el texto sea mucho más corto.
   */
  uint8_t bufferSalida[TAMANO_PAQUETE_SPI];

  // Rellena todo el bloque con ceros.
  memset(
    bufferSalida,
    0,
    sizeof(bufferSalida)
  );

  // Copia el texto y deja siempre sitio para el '\0'.
  strncpy(
    reinterpret_cast<char*>(bufferSalida),
    texto,
    TAMANO_PAQUETE_SPI - 1
  );

  bufferSalida[TAMANO_PAQUETE_SPI - 1] = '\0';

  uint8_t bufferEntrada[TAMANO_PAQUETE_SPI];

  SPI.beginTransaction(configuracionSPI);

  digitalWrite(PIN_CS_HELTEC, LOW);

  /*
   * Se intercambia el bloque entero manteniendo CS en LOW durante toda
   * la transacción.
   */
  for (size_t i = 0; i < TAMANO_PAQUETE_SPI; i++)
  {
    bufferEntrada[i] = SPI.transfer(bufferSalida[i]);
  }

  digitalWrite(PIN_CS_HELTEC, HIGH);

  SPI.endTransaction();

  procesarRecepcionSPI(bufferEntrada, sizeof(bufferEntrada));
}


// =====================================================
// RECEPCIÓN SPI
// =====================================================

/*
 * Trata el bloque que acaba de llegar de la Heltec.
 *
 * La mayoría de los intercambios no traen nada nuevo, así que casi
 * todas las llamadas terminan descartando el bloque. Solo las líneas
 * nuevas y con pinta de dato llegan al puerto serie.
 */
void procesarRecepcionSPI(const uint8_t* datos, size_t capacidad)
{
  /*
   * Se copia con terminador propio: no hay ninguna garantía de que lo
   * que venga del bus esté bien cerrado, y strlen() sobre un buffer
   * sin '\0' se saldría del array.
   */
  memcpy(recepcionSPI, datos, capacidad - 1);

  recepcionSPI[capacidad - 1] = '\0';

  if (!esLineaSPIValida(recepcionSPI))
  {
    return;
  }

  // Ver el comentario de ultimaLineaSPI: la Heltec repite el bloque
  // hasta que tiene algo nuevo que decir.
  if (strcmp(recepcionSPI, ultimaLineaSPI) == 0)
  {
    return;
  }

  strncpy(
    ultimaLineaSPI,
    recepcionSPI,
    sizeof(ultimaLineaSPI) - 1
  );

  ultimaLineaSPI[sizeof(ultimaLineaSPI) - 1] = '\0';

  lineasRecibidasSPI++;

  if (!enlaceSPIConfirmado)
  {
    enlaceSPIConfirmado = true;

    Serial.println("$SYS,ENLACE_SPI_CONFIRMADO");
  }

  // La línea de la Heltec sale tal cual, con su propio prefijo.
  Serial.println(recepcionSPI);

  /*
   * Y se reenvía a la app, que es quien pinta estas líneas.
   *
   * Va tal cual, sin traducir: la app enruta por el prefijo, así que
   * reconoce "Lec:" igual que reconoce "$ADC" o "$MET". Lo que no
   * entienda lo ignora, de modo que añadir fuentes nuevas aquí no la
   * rompe.
   */
  if (BLE.connected())
  {
    enviarPorBLE(recepcionSPI);
  }
}


/*
 * Filtro de lo que llega por MISO.
 *
 * Hace falta porque el Portenta clockea el bus en cada envío, tenga la
 * Heltec algo que decir o no. Sin filtro se imprimirían bloques vacíos
 * varias veces por segundo, y basura constante si el cable de MISO no
 * está conectado.
 */
bool esLineaSPIValida(const char* linea)
{
  // Bloque vacío: la Heltec no tenía nada pendiente.
  if (linea[0] == '\0')
  {
    return false;
  }

  /*
   * Un 0xFF de entrada es lo que se lee cuando nadie gobierna MISO:
   * la línea se queda al aire. No es un dato.
   */
  if (static_cast<uint8_t>(linea[0]) == 0xFF)
  {
    return false;
  }

  size_t longitud = strlen(linea);

  if (longitud < LONGITUD_MINIMA_LINEA_SPI)
  {
    return false;
  }

  /*
   * Todo lo que mandan las Heltec es texto. Si aparece un byte no
   * imprimible es ruido del bus o una transacción desalineada, no una
   * línea de datos.
   */
  for (size_t i = 0; i < longitud; i++)
  {
    if (!isprint(static_cast<unsigned char>(linea[i])))
    {
      return false;
    }
  }

  return true;
}


// =====================================================
// COMANDOS (Serial y BLE)
// =====================================================

/*
 * Revisa si "mensaje" es el comando comvel:<ms>.
 *
 * Devuelve true si el mensaje fue reconocido como este
 * comando (válido o inválido); en ese caso el llamador
 * no debe tratarlo como un mensaje manual normal.
 *
 * Devuelve false si el mensaje no tiene el prefijo
 * "comvel:", por lo que debe seguir su curso habitual.
 */
bool intentarProcesarComandoVelocidad(const char* mensaje)
{
  size_t longitudPrefijo = strlen(PREFIJO_COMANDO_VELOCIDAD);

  if (
    strncmp(
      mensaje,
      PREFIJO_COMANDO_VELOCIDAD,
      longitudPrefijo
    ) != 0
  )
  {
    return false;
  }

  const char* textoValor = mensaje + longitudPrefijo;

  if (textoValor[0] == '\0')
  {
    Serial.println("ERROR: comvel sin valor. Uso: comvel:<ms>");
    return true;
  }

  for (const char* p = textoValor; *p != '\0'; p++)
  {
    if (!isdigit(static_cast<unsigned char>(*p)))
    {
      Serial.println(
        "ERROR: comvel espera un valor numerico en milisegundos."
      );
      return true;
    }
  }

  long valorSolicitado = atol(textoValor);

  if (
    valorSolicitado < static_cast<long>(PERIODO_ADC_MS_MIN) ||
    valorSolicitado > static_cast<long>(PERIODO_ADC_MS_MAX)
  )
  {
    Serial.print("ERROR: comvel fuera de rango (");
    Serial.print(PERIODO_ADC_MS_MIN);
    Serial.print(" - ");
    Serial.print(PERIODO_ADC_MS_MAX);
    Serial.println(" ms).");
    return true;
  }

  periodoADCMs = static_cast<unsigned long>(valorSolicitado);

  Serial.print("Periodo ADC actualizado a ");
  Serial.print(periodoADCMs);
  Serial.println(" ms.");

  return true;
}


// =====================================================
// ENTRADA MANUAL DESDE EL MONITOR SERIE
// =====================================================

void procesarEntradaSerial()
{
  while (Serial.available() > 0)
  {
    char caracter = Serial.read();

    if (caracter == '\n')
    {
      if (posicionEntradaSerial > 0)
      {
        entradaSerial[posicionEntradaSerial] = '\0';

        procesarMensajeEntrante(entradaSerial, "Serial");

        posicionEntradaSerial = 0;
      }
    }
    else if (caracter != '\r')
    {
      if (
        posicionEntradaSerial <
        sizeof(entradaSerial) - 1
      )
      {
        entradaSerial[posicionEntradaSerial] = caracter;
        posicionEntradaSerial++;
      }
      else
      {
        Serial.println(
          "ERROR: mensaje manual demasiado largo."
        );

        posicionEntradaSerial = 0;
      }
    }
  }
}


// =====================================================
// ESTADO DE CONEXIÓN BLE
// =====================================================

void actualizarEstadoBLE()
{
  bool conectadoAhora = BLE.connected();

  if (conectadoAhora && !estadoBLEAnterior)
  {
    Serial.println();
    Serial.println("Dispositivo BLE conectado.");
    Serial.println("Transmision BLE activada.");
    Serial.println();
  }
  else if (!conectadoAhora && estadoBLEAnterior)
  {
    Serial.println();
    Serial.println("Dispositivo BLE desconectado.");
    Serial.println("Serial y SPI continuan activos.");
    Serial.println();
  }

  estadoBLEAnterior = conectadoAhora;
}


// =====================================================
// TRATAMIENTO COMÚN DE MENSAJES (Serial y BLE)
// =====================================================

/*
 * Único punto por el que pasan las líneas completas, vengan del
 * Monitor Serie o de la app por BLE.
 *
 * Primero se comprueba si la línea es un comando. Si no lo es, se
 * conserva el comportamiento de siempre: reenviarla como mensaje
 * manual por SPI y por BLE.
 */
void procesarMensajeEntrante(
  const char* mensaje,
  const char* origen
)
{
  if (intentarProcesarComandoVelocidad(mensaje))
  {
    return;
  }

  if (intentarProcesarComandoHora(mensaje))
  {
    return;
  }

  Serial.print("Mensaje manual (");
  Serial.print(origen);
  Serial.print("): ");
  Serial.println(mensaje);

  // El mensaje manual se comparte por SPI.
  enviarPorSPI(mensaje);

  /*
   * Y por BLE, si existe una conexión.
   *
   * Cuando el mensaje viene de la propia app, esto le sirve de acuse
   * de recibo: TX y RX son características distintas, así que no se
   * forma ningún bucle.
   */
  if (BLE.connected())
  {
    enviarPorBLE(mensaje);
  }
}


// =====================================================
// RECEPCIÓN BLE
// =====================================================

/*
 * Se ejecuta cada vez que la app escribe en la característica RX.
 *
 * Aquí solo se acumulan bytes. El trabajo de verdad lo hace loop(),
 * porque este manejador corre desde dentro de BLE.poll().
 */
void alRecibirEscrituraBLE(
  BLEDevice central,
  BLECharacteristic caracteristica
)
{
  (void)central;

  int cantidad = caracteristica.valueLength();
  const uint8_t* datos = caracteristica.value();

  tiempoUltimoFragmentoBLE = millis();

  for (int i = 0; i < cantidad; i++)
  {
    char caracter = static_cast<char>(datos[i]);

    if (caracter == '\n')
    {
      cerrarLineaBLE();
    }
    else if (caracter != '\r')
    {
      if (posicionEntradaBLE < sizeof(entradaBLE) - 1)
      {
        entradaBLE[posicionEntradaBLE] = caracter;
        posicionEntradaBLE++;
      }
      else
      {
        Serial.println("ERROR: mensaje BLE demasiado largo.");

        posicionEntradaBLE = 0;
      }
    }
  }
}


/*
 * Da por terminada la línea que se estaba acumulando y la deja
 * pendiente para loop().
 */
void cerrarLineaBLE()
{
  if (posicionEntradaBLE == 0)
  {
    return;
  }

  entradaBLE[posicionEntradaBLE] = '\0';

  /*
   * Si loop() todavía no ha atendido la línea anterior, esta se
   * descarta: es preferible perder un comando que machacar el buffer
   * mientras se está leyendo.
   */
  if (hayMensajeBLEPendiente)
  {
    Serial.println(
      "AVISO: linea BLE descartada, la anterior sigue en cola."
    );

    posicionEntradaBLE = 0;
    return;
  }

  strncpy(
    mensajeBLEPendiente,
    entradaBLE,
    sizeof(mensajeBLEPendiente) - 1
  );

  mensajeBLEPendiente[sizeof(mensajeBLEPendiente) - 1] = '\0';

  hayMensajeBLEPendiente = true;
  posicionEntradaBLE = 0;
}


/*
 * Corta por tiempo una línea a medias. Sin esto, una app que no envíe
 * '\n' dejaría el comando esperando indefinidamente.
 */
void revisarTiempoLimiteLineaBLE()
{
  if (posicionEntradaBLE == 0 || hayMensajeBLEPendiente)
  {
    return;
  }

  if (
    millis() - tiempoUltimoFragmentoBLE >=
    TIEMPO_LIMITE_LINEA_BLE_MS
  )
  {
    cerrarLineaBLE();
  }
}


// =====================================================
// ACCESO AL DS3231
// =====================================================

// El DS3231 guarda sus registros en BCD: cada nibble es una cifra.
uint8_t bcdABinario(uint8_t valor)
{
  return static_cast<uint8_t>((valor >> 4) * 10 + (valor & 0x0F));
}


uint8_t binarioABcd(uint8_t valor)
{
  return static_cast<uint8_t>(((valor / 10) << 4) | (valor % 10));
}


/*
 * Lectura de registros consecutivos.
 *
 * Devuelve false ante cualquier problema del bus, para que el que
 * llama no se quede con datos a medias.
 */
bool leerRegistrosRTC(
  uint8_t registroInicial,
  uint8_t* destino,
  uint8_t cantidad
)
{
  Wire.beginTransmission(DIRECCION_I2C_DS3231);
  Wire.write(registroInicial);

  if (Wire.endTransmission() != 0)
  {
    return false;
  }

  if (Wire.requestFrom(DIRECCION_I2C_DS3231, cantidad) != cantidad)
  {
    return false;
  }

  for (uint8_t i = 0; i < cantidad; i++)
  {
    if (Wire.available() == 0)
    {
      return false;
    }

    destino[i] = static_cast<uint8_t>(Wire.read());
  }

  return true;
}


bool escribirRegistroRTC(uint8_t registro, uint8_t valor)
{
  Wire.beginTransmission(DIRECCION_I2C_DS3231);
  Wire.write(registro);
  Wire.write(valor);

  return Wire.endTransmission() == 0;
}


bool leerFechaHoraRTC(FechaHora& destino)
{
  uint8_t registros[NUM_REGISTROS_FECHA_HORA];

  if (
    !leerRegistrosRTC(
      REG_DS3231_SEGUNDOS,
      registros,
      NUM_REGISTROS_FECHA_HORA
    )
  )
  {
    return false;
  }

  destino.segundo = bcdABinario(registros[0] & 0x7F);
  destino.minuto  = bcdABinario(registros[1] & 0x7F);

  /*
   * Bit 6 del registro de horas: 1 = formato de 12 horas.
   *
   * Este programa siempre escribe en formato de 24 horas, pero el
   * módulo puede venir de fábrica o de otro montaje en formato de 12,
   * así que se contemplan los dos casos.
   */
  if (registros[2] & 0x40)
  {
    uint8_t horas12 = bcdABinario(registros[2] & 0x1F);

    bool esPasadoMediodia = (registros[2] & 0x20) != 0;

    // Las 12 AM son las 0 h y las 12 PM son las 12 h.
    if (horas12 == 12)
    {
      horas12 = 0;
    }

    destino.hora =
      esPasadoMediodia
        ? static_cast<uint8_t>(horas12 + 12)
        : horas12;
  }
  else
  {
    destino.hora = bcdABinario(registros[2] & 0x3F);
  }

  destino.diaSemana = static_cast<uint8_t>(registros[3] & 0x07);
  destino.dia       = bcdABinario(registros[4] & 0x3F);

  // Bit 7 del registro de mes es el bit de siglo, que aquí se ignora.
  destino.mes       = bcdABinario(registros[5] & 0x1F);

  destino.anio =
    static_cast<uint16_t>(2000 + bcdABinario(registros[6]));

  return true;
}


bool escribirFechaHoraRTC(const FechaHora& nueva)
{
  Wire.beginTransmission(DIRECCION_I2C_DS3231);
  Wire.write(REG_DS3231_SEGUNDOS);

  Wire.write(binarioABcd(nueva.segundo));
  Wire.write(binarioABcd(nueva.minuto));

  // Bit 6 a 0: se fija el formato de 24 horas.
  Wire.write(binarioABcd(nueva.hora));

  Wire.write(nueva.diaSemana);
  Wire.write(binarioABcd(nueva.dia));

  // Bit 7 (siglo) a 0: se trabaja siempre en 20xx.
  Wire.write(binarioABcd(nueva.mes));

  Wire.write(
    binarioABcd(static_cast<uint8_t>(nueva.anio - 2000))
  );

  if (Wire.endTransmission() != 0)
  {
    return false;
  }

  /*
   * Una vez puesto en hora, la marca de "el oscilador se detuvo" ya no
   * tiene sentido: hay que borrarla a mano, el DS3231 no lo hace solo.
   */
  return limpiarBanderaOsciladorDetenido();
}


bool leerBanderaOsciladorDetenido(bool& detenido)
{
  uint8_t estado = 0;

  if (!leerRegistrosRTC(REG_DS3231_ESTADO, &estado, 1))
  {
    return false;
  }

  detenido = (estado & MASCARA_OSF) != 0;

  return true;
}


bool limpiarBanderaOsciladorDetenido()
{
  uint8_t estado = 0;

  if (!leerRegistrosRTC(REG_DS3231_ESTADO, &estado, 1))
  {
    return false;
  }

  estado = static_cast<uint8_t>(estado & ~MASCARA_OSF);

  return escribirRegistroRTC(REG_DS3231_ESTADO, estado);
}


/*
 * Temperatura del sensor interno del DS3231, en cuartos de grado.
 *
 * Se devuelve en cuartos y no en float a propósito: el sensor tiene
 * exactamente esa resolución (0,25 °C) y así se evita arrastrar coma
 * flotante hasta el snprintf.
 */
bool leerTemperaturaRTC(int16_t& cuartosDeGrado)
{
  uint8_t registros[2];

  if (!leerRegistrosRTC(REG_DS3231_TEMP_MSB, registros, 2))
  {
    return false;
  }

  // El byte alto es la parte entera con signo, en complemento a dos.
  int8_t parteEntera = static_cast<int8_t>(registros[0]);

  // Solo los dos bits altos del byte bajo cuentan.
  uint8_t fraccion = static_cast<uint8_t>(registros[1] >> 6);

  cuartosDeGrado =
    static_cast<int16_t>(
      static_cast<int16_t>(parteEntera) * 4 + fraccion
    );

  return true;
}


void formatearTemperaturaRTC(char* destino, size_t capacidad)
{
  int16_t cuartosDeGrado = 0;

  if (!leerTemperaturaRTC(cuartosDeGrado))
  {
    snprintf(destino, capacidad, "NA");
    return;
  }

  // Cada cuenta son 0,25 °C, es decir, 25 centésimas.
  int32_t centesimas = static_cast<int32_t>(cuartosDeGrado) * 25;

  const char* signo = (centesimas < 0) ? "-" : "";

  if (centesimas < 0)
  {
    centesimas = -centesimas;
  }

  snprintf(
    destino,
    capacidad,
    "%s%ld.%02ld",
    signo,
    static_cast<long>(centesimas / 100),
    static_cast<long>(centesimas % 100)
  );
}


// =====================================================
// FECHAS: VALIDACIÓN Y DÍA DE LA SEMANA
// =====================================================

bool esAnioBisiesto(int anio)
{
  return (anio % 4 == 0 && anio % 100 != 0) || (anio % 400 == 0);
}


uint8_t diasDelMes(int anio, int mes)
{
  static const uint8_t dias[] = {
    31, 28, 31, 30, 31, 30,
    31, 31, 30, 31, 30, 31
  };

  if (mes == 2 && esAnioBisiesto(anio))
  {
    return 29;
  }

  return dias[mes - 1];
}


bool esFechaHoraValida(
  int anio,
  int mes,
  int dia,
  int hora,
  int minuto,
  int segundo
)
{
  if (anio < ANIO_MINIMO_RTC || anio > ANIO_MAXIMO_RTC)
  {
    return false;
  }

  if (mes < 1 || mes > 12)
  {
    return false;
  }

  if (dia < 1 || dia > static_cast<int>(diasDelMes(anio, mes)))
  {
    return false;
  }

  if (hora < 0 || hora > 23)
  {
    return false;
  }

  if (minuto < 0 || minuto > 59)
  {
    return false;
  }

  if (segundo < 0 || segundo > 59)
  {
    return false;
  }

  return true;
}


/*
 * Día de la semana a partir de la fecha, por el método de Sakamoto.
 *
 * Se calcula en lugar de pedirlo para que el comando comhora no tenga
 * que llevar un dato que ya está implícito en la fecha.
 */
uint8_t calcularDiaSemana(int anio, int mes, int dia)
{
  static const int desplazamiento[] = {
    0, 3, 2, 5, 0, 3,
    5, 1, 4, 6, 2, 4
  };

  int a = anio;

  if (mes < 3)
  {
    a -= 1;
  }

  int indice =
    (a + a / 4 - a / 100 + a / 400 + desplazamiento[mes - 1] + dia) % 7;

  // Sakamoto da 0 = domingo; el DS3231 espera de 1 a 7.
  return static_cast<uint8_t>(indice + 1);
}


// =====================================================
// INICIALIZACIÓN DEL RTC
// =====================================================

void iniciarRTC()
{
  Wire.begin();

  /*
   * 100 kHz. El DS3231 admite 400 kHz, pero con los pull-up de 4,7 k
   * del módulo y cables sueltos de protoboard, 100 kHz da menos
   * disgustos y aquí no hace falta más.
   */
  Wire.setClock(100000);

  Serial.println("Iniciando RTC DS3231...");

  Wire.beginTransmission(DIRECCION_I2C_DS3231);

  if (Wire.endTransmission() != 0)
  {
    rtcPresente = false;
    avisoErrorRTCMostrado = true;

    Serial.println("ERROR: el DS3231 no responde en 0x68.");
    Serial.println("Revisar SDA=D11, SCL=D12, GND comun y VCC a 3V3.");
    return;
  }

  rtcPresente = true;

  Serial.println("DS3231 detectado en 0x68.");
  Serial.println("SDA: D11 (PH_8)");
  Serial.println("SCL: D12 (PH_7)");

  bool osciladorDetenido = false;

  if (
    leerBanderaOsciladorDetenido(osciladorDetenido) &&
    osciladorDetenido
  )
  {
    avisoHoraNoValidaMostrado = true;

    Serial.println(
      "AVISO: el oscilador se detuvo. La hora NO es valida."
    );
    Serial.println(
      "Poner en hora con: comhora:AAAA-MM-DD hh:mm:ss"
    );
  }

  FechaHora ahora;

  if (leerFechaHoraRTC(ahora))
  {
    char linea[64];

    snprintf(
      linea,
      sizeof(linea),
      "Hora actual del RTC: %04u-%02u-%02u %02u:%02u:%02u",
      static_cast<unsigned>(ahora.anio),
      static_cast<unsigned>(ahora.mes),
      static_cast<unsigned>(ahora.dia),
      static_cast<unsigned>(ahora.hora),
      static_cast<unsigned>(ahora.minuto),
      static_cast<unsigned>(ahora.segundo)
    );

    Serial.println(linea);
  }
}


// =====================================================
// PUBLICACIÓN DE LA HORA
// =====================================================

/*
 * Publica la hora por los tres caminos: puerto serie, SPI y BLE.
 *
 * Va por distribuirPaquete(), igual que el paquete del ADC, para que
 * la Heltec reciba la marca de tiempo además de las lecturas.
 *
 * Efecto secundario útil: como esto se ejecuta una vez por segundo
 * pase lo que pase, garantiza al menos un intercambio SPI por segundo
 * aunque se suba mucho el periodo del ADC con comvel:. Y cada
 * intercambio es una oportunidad de recoger lo que la Heltec tenga
 * preparado en MISO.
 */
void publicarFechaHora()
{
  FechaHora ahora;

  if (!leerFechaHoraRTC(ahora))
  {
    rtcPresente = false;

    // Solo se avisa al pasar a fallo, no una vez por segundo.
    if (!avisoErrorRTCMostrado)
    {
      avisoErrorRTCMostrado = true;

      Serial.println("$RTC,ERROR:sin_respuesta_I2C");
    }

    return;
  }

  if (!rtcPresente)
  {
    Serial.println("$RTC,INFO:comunicacion_recuperada");
  }

  rtcPresente = true;
  avisoErrorRTCMostrado = false;

  char textoTemperatura[16];

  formatearTemperaturaRTC(
    textoTemperatura,
    sizeof(textoTemperatura)
  );

  char paqueteRTC[96];

  snprintf(
    paqueteRTC,
    sizeof(paqueteRTC),
    "$RTC,"
    "FECHA:%04u-%02u-%02u,"
    "HORA:%02u:%02u:%02u,"
    "DIA:%u,"
    "TEMP:%s",
    static_cast<unsigned>(ahora.anio),
    static_cast<unsigned>(ahora.mes),
    static_cast<unsigned>(ahora.dia),
    static_cast<unsigned>(ahora.hora),
    static_cast<unsigned>(ahora.minuto),
    static_cast<unsigned>(ahora.segundo),
    static_cast<unsigned>(ahora.diaSemana),
    textoTemperatura
  );

  distribuirPaquete(paqueteRTC);

  /*
   * Si el oscilador se detuvo, la línea que se acaba de imprimir no
   * vale. El aviso se da una sola vez, hasta que se ponga en hora.
   */
  bool osciladorDetenido = false;

  if (leerBanderaOsciladorDetenido(osciladorDetenido))
  {
    if (osciladorDetenido && !avisoHoraNoValidaMostrado)
    {
      avisoHoraNoValidaMostrado = true;

      Serial.println(
        "$RTC,AVISO:hora_no_valida usar comhora:AAAA-MM-DD hh:mm:ss"
      );
    }
    else if (!osciladorDetenido)
    {
      avisoHoraNoValidaMostrado = false;
    }
  }
}


// =====================================================
// COMANDO comhora:AAAA-MM-DD hh:mm:ss
// =====================================================

/*
 * Misma idea que intentarProcesarComandoVelocidad: devuelve true si la
 * línea era este comando, válido o no, para que el que llama no la
 * trate además como mensaje manual.
 */
bool intentarProcesarComandoHora(const char* mensaje)
{
  size_t longitudPrefijo = strlen(PREFIJO_COMANDO_HORA);

  if (
    strncmp(
      mensaje,
      PREFIJO_COMANDO_HORA,
      longitudPrefijo
    ) != 0
  )
  {
    return false;
  }

  const char* textoValor = mensaje + longitudPrefijo;

  int anio = 0;
  int mes = 0;
  int dia = 0;
  int hora = 0;
  int minuto = 0;
  int segundo = 0;

  int camposLeidos = sscanf(
    textoValor,
    "%d-%d-%d %d:%d:%d",
    &anio, &mes, &dia,
    &hora, &minuto, &segundo
  );

  if (camposLeidos != 6)
  {
    Serial.println(
      "ERROR: uso comhora:AAAA-MM-DD hh:mm:ss"
    );

    return true;
  }

  if (!esFechaHoraValida(anio, mes, dia, hora, minuto, segundo))
  {
    Serial.println(
      "ERROR: fecha u hora fuera de rango."
    );

    return true;
  }

  FechaHora nueva;

  nueva.anio      = static_cast<uint16_t>(anio);
  nueva.mes       = static_cast<uint8_t>(mes);
  nueva.dia       = static_cast<uint8_t>(dia);
  nueva.hora      = static_cast<uint8_t>(hora);
  nueva.minuto    = static_cast<uint8_t>(minuto);
  nueva.segundo   = static_cast<uint8_t>(segundo);
  nueva.diaSemana = calcularDiaSemana(anio, mes, dia);

  if (!escribirFechaHoraRTC(nueva))
  {
    Serial.println(
      "ERROR: no fue posible escribir en el DS3231."
    );

    return true;
  }

  avisoHoraNoValidaMostrado = false;

  char confirmacion[80];

  snprintf(
    confirmacion,
    sizeof(confirmacion),
    "RTC puesto en hora: %04d-%02d-%02d %02d:%02d:%02d (dia %u)",
    anio, mes, dia,
    hora, minuto, segundo,
    static_cast<unsigned>(nueva.diaSemana)
  );

  Serial.println(confirmacion);

  return true;
}
