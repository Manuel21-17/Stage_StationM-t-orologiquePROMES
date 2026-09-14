using System;
using System.Collections.Generic;
using Xamarin.Essentials;

namespace STageAPP_1
{
    /*
     * Qué sensor hay conectado a cada entrada.
     *
     * Es el punto de encuentro entre "Configurar sensores" (que
     * escribe) y "Datos eléctricos" (que lee para decidir el nombre,
     * la unidad y el color de cada gráfica).
     *
     * La asignación se guarda con Preferences, así que sobrevive a
     * cerrar la aplicación: al volver a abrirla, cada selector y cada
     * gráfica recuperan lo último que eligió el usuario.
     */
    public static class ConfiguracionSensores
    {
        public const int NumeroDeEntradas = 23;

        // Las 7 primeras entradas son del propio Portenta; el resto
        // son de la tarjeta analógica.
        public const int EntradasPortenta = 7;

        private const string PrefijoClave = "sensor_entrada_";

        /*
         * Orden en el que se ofrecen los tipos al usuario. El
         * desplegable y la traducción índice -> tipo salen de este
         * mismo array, para que no puedan desincronizarse.
         *
         * SinAsignar no aparece: es el estado inicial, no una opción
         * que se pueda elegir.
         */
        public static readonly TipoSensor[] TiposSeleccionables =
        {
            TipoSensor.Meteorologico,
            TipoSensor.Corriente,
            TipoSensor.Voltaje
        };

        // Avisa a las pantallas abiertas de que una asignación cambió.
        public static event EventHandler AsignacionesCambiadas;

        public static TipoSensor Obtener(int numeroEntrada)
        {
            if (!EsEntradaValida(numeroEntrada))
                return TipoSensor.SinAsignar;

            int valorGuardado = Preferences.Get(
                Clave(numeroEntrada),
                (int)TipoSensor.SinAsignar);

            /*
             * Se valida lo leído del disco: un valor corrupto o de una
             * versión anterior no debe convertirse en un enum inválido
             * que luego reviente al pintar.
             */
            if (!Enum.IsDefined(typeof(TipoSensor), valorGuardado))
                return TipoSensor.SinAsignar;

            return (TipoSensor)valorGuardado;
        }

        public static void Asignar(
            int numeroEntrada,
            TipoSensor tipo)
        {
            if (!EsEntradaValida(numeroEntrada))
                return;

            if (Obtener(numeroEntrada) == tipo)
                return;

            Preferences.Set(
                Clave(numeroEntrada),
                (int)tipo);

            AsignacionesCambiadas?.Invoke(null, EventArgs.Empty);
        }

        public static int IndiceDeTipo(TipoSensor tipo)
        {
            for (int i = 0; i < TiposSeleccionables.Length; i++)
            {
                if (TiposSeleccionables[i] == tipo)
                    return i;
            }

            // Sin asignar: el desplegable se queda en su título.
            return -1;
        }

        /*
         * Nombre de la entrada. Vive aquí porque lo necesitan las dos
         * pantallas y antes estaba escrito por duplicado en cada una.
         */
        public static string NombreEntrada(int numeroEntrada)
        {
            if (numeroEntrada <= EntradasPortenta)
            {
                return "Entrée " + numeroEntrada + " Portenta";
            }

            int numeroAnalogico = numeroEntrada - EntradasPortenta;

            return "Entrée " + numeroAnalogico + " Analog";
        }

        // Texto del tipo tal y como se muestra en las gráficas.
        public static string Descripcion(TipoSensor tipo)
        {
            switch (tipo)
            {
                case TipoSensor.Meteorologico:
                    return "Météorologique";

                case TipoSensor.Corriente:
                    return "Courant";

                case TipoSensor.Voltaje:
                    return "Tension";

                default:
                    return "Non affecté";
            }
        }

        // Texto del tipo tal y como se ofrece en el desplegable.
        public static string DescripcionSeleccionable(TipoSensor tipo)
        {
            switch (tipo)
            {
                case TipoSensor.Meteorologico:
                    return "Capteur météorologique";

                case TipoSensor.Corriente:
                    return "Capteur de courant";

                case TipoSensor.Voltaje:
                    return "Capteur de tension";

                default:
                    return "Non affecté";
            }
        }

        // Clave de la paleta de App.xaml con la que se pinta la línea.
        public static string ClaveColor(TipoSensor tipo)
        {
            switch (tipo)
            {
                case TipoSensor.Meteorologico:
                    return "ColorMeteorologico";

                case TipoSensor.Corriente:
                    return "ColorCorriente";

                case TipoSensor.Voltaje:
                    return "ColorTension";

                default:
                    return "ColorTextoSecundario";
            }
        }

        // ============================================================
        // Escalas de las gráficas
        // ============================================================

        /*
         * ESPEJO de las rectas de calibración del firmware.
         *
         * El Portenta ya no manda la cuenta cruda del ADC: aplica
         * y = x·pendiente + ordenada según el tipo configurado y manda
         * el valor convertido. Estos topes son el resultado de aplicar
         * cada recta a la lectura máxima (4095), redondeado a una cifra
         * que dé divisiones limpias:
         *
         *   voltaje      4095/30 + 2 = 138.5  -> escala 0..150
         *   corriente    4095/40 + 3 = 105.4  -> escala 0..120
         *   meteorológico 4095/10 + 5 = 414.5 -> escala 0..500
         *   sin asignar  cuenta cruda         -> escala 0..4096
         *
         * ATENCIÓN: si se cambian los coeficientes en el firmware
         * (CALIBRACION en firmware_portenta_v2.ino), hay que revisar
         * estos topes. Es el único punto donde la aplicación y el
         * firmware comparten conocimiento de las rectas.
         */
        public static float MaximoEscala(TipoSensor tipo)
        {
            switch (tipo)
            {
                case TipoSensor.Voltaje:
                    return 150f;

                case TipoSensor.Corriente:
                    return 120f;

                case TipoSensor.Meteorologico:
                    return 500f;

                default:
                    return 4096f;
            }
        }

        // Divisiones elegidas para que las etiquetas del eje salgan
        // redondas: 30, 30, 100 y 1024 respectivamente.
        public static int DivisionesEscala(TipoSensor tipo)
        {
            switch (tipo)
            {
                case TipoSensor.Voltaje:
                    return 5;

                case TipoSensor.Corriente:
                    return 4;

                case TipoSensor.Meteorologico:
                    return 5;

                default:
                    return 4;
            }
        }

        public static string Unidad(TipoSensor tipo)
        {
            switch (tipo)
            {
                case TipoSensor.Corriente:
                    return " A";

                case TipoSensor.Voltaje:
                    return " V";

                default:
                    /*
                     * Un sensor meteorológico en una entrada analógica
                     * entrega la cuenta cruda del ADC; ponerle voltios
                     * o amperios sería mentir sobre la magnitud.
                     */
                    return string.Empty;
            }
        }

        // ============================================================
        // Comando de configuración hacia el Portenta
        // ============================================================

        public const string PrefijoComandoConfiguracion = "comConf:";

        /*
         * Etiqueta de la entrada dentro del comando: P1..P7 para las
         * del Portenta y A1..A16 para las de la tarjeta analógica.
         */
        public static string EtiquetaEntrada(int numeroEntrada)
        {
            if (numeroEntrada <= EntradasPortenta)
            {
                return "P" + numeroEntrada;
            }

            return "A" + (numeroEntrada - EntradasPortenta);
        }

        public static int ContarAsignadas()
        {
            int cuenta = 0;

            for (int n = 1; n <= NumeroDeEntradas; n++)
            {
                if (Obtener(n) != TipoSensor.SinAsignar)
                    cuenta++;
            }

            return cuenta;
        }

        /*
         * Construye el comando de configuración, troceado en mensajes
         * que quepan en la característica RX.
         *
         * El comando completo con las 23 entradas ocupa 129 bytes y la
         * característica solo admite 20, así que un único envío es
         * imposible. Cada trozo se emite como un comConf: completo y
         * autónomo (caben dos entradas por mensaje), de modo que el
         * firmware puede aplicar cada uno según llega, sin necesidad de
         * reensamblar nada ni de saber cuántos faltan.
         *
         * Las entradas sin asignar se omiten por completo: no se envía
         * ningún hueco ni valor de relleno.
         */
        public static List<string> ConstruirComandosConfiguracion()
        {
            List<string> comandos = new List<string>();

            string enCurso = null;

            for (int n = 1; n <= NumeroDeEntradas; n++)
            {
                TipoSensor tipo = Obtener(n);

                if (tipo == TipoSensor.SinAsignar)
                    continue;

                // Los valores del enum son 1, 2 y 3, que es justo lo
                // que espera el firmware.
                string token =
                    EtiquetaEntrada(n) + ":" + (int)tipo;

                if (enCurso == null)
                {
                    enCurso = PrefijoComandoConfiguracion + token;
                    continue;
                }

                string ampliado = enCurso + "," + token;

                /*
                 * Todo el comando es ASCII (letras, dígitos, ':' y
                 * ','), así que la longitud en caracteres coincide con
                 * la longitud en bytes.
                 */
                if (ampliado.Length <=
                    BluetoothManager.MaximoBytesComando)
                {
                    enCurso = ampliado;
                }
                else
                {
                    comandos.Add(enCurso);

                    enCurso = PrefijoComandoConfiguracion + token;
                }
            }

            if (enCurso != null)
                comandos.Add(enCurso);

            return comandos;
        }

        private static bool EsEntradaValida(int numeroEntrada)
        {
            return numeroEntrada >= 1 &&
                   numeroEntrada <= NumeroDeEntradas;
        }

        private static string Clave(int numeroEntrada)
        {
            return PrefijoClave + numeroEntrada;
        }
    }
}
