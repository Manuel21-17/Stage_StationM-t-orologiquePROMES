using System;
using System.Globalization;
using System.Threading.Tasks;
using Xamarin.Forms;

namespace STageAPP_1
{
    public partial class ConfiguracionPortentaPage : ContentPage
    {
        /*
         * Comandos que espera el firmware del PortentaH7:
         *
         *   comvel:#            intervalo entre muestras en ms
         *   comPS:True|False    emisión de paquetes por puerto serie
         */
        private const string PrefijoComandoVelocidad = "comvel:";
        private const string PrefijoComandoSerial = "comPS:";

        /*
         * Puesta en hora del RTC DS3231:
         *
         *   comhora:AAAA-MM-DD hh:mm:ss
         *
         * El DS3231 no tiene red ni noción de zona horaria: guarda la
         * hora de pared que se le escriba. Se le manda DateTime.Now,
         * es decir, la hora local del teléfono.
         */
        private const string PrefijoComandoHora = "comhora:";

        // Formato exacto que espera el firmware. HH es de 24 horas.
        private const string FormatoHora = "yyyy-MM-dd HH:mm:ss";

        private static readonly int[] VelocidadesMs =
        {
            5, 10, 20, 50, 100, 200, 500, 1000, 5000
        };

        // Claves de la paleta definida en App.xaml.
        private const string ColorNeutro = "ColorTextoSecundario";
        private const string ColorExito = "ColorIndicadorActivo";
        private const string ColorError = "ColorIndicadorError";

        /*
         * Recorrido del pulgar del interruptor.
         *
         * Pista de 58, pulgar de 24 y margen de 4 a cada lado:
         * 58 - 24 - 4 - 4 = 26.
         */
        private const double RecorridoPulgar = 26;

        private const uint DuracionAnimacionMs = 160;

        /*
         * Asignar SelectedIndex por código también dispara
         * SelectedIndexChanged. Esta bandera evita que restablecer el
         * selector tras un fallo se interprete como una nueva
         * selección del usuario y vuelva a enviar el comando.
         */
        private bool ignorarCambioSeleccion;

        /*
         * Estado del interruptor. Arranca en true porque el firmware
         * emite por serie tras cada reset o encendido.
         */
        private bool salidaSerialActiva = true;

        private bool cambioSerialEnCurso;

        // Evita que el envío automático al abrir la página y un toque
        // del botón se pisen y manden dos horas seguidas.
        private bool sincronizacionHoraEnCurso;

        public ConfiguracionPortentaPage()
        {
            InitializeComponent();

            LlenarSelectorVelocidad();

            ColocarInterruptor(salidaSerialActiva);
        }

        private void LlenarSelectorVelocidad()
        {
            foreach (int velocidad in VelocidadesMs)
            {
                SelectorVelocidad.Items.Add(
                    velocidad.ToString(CultureInfo.InvariantCulture) +
                    " ms/donnée");
            }
        }

        // ============================================================
        // Velocidad de recolección
        // ============================================================

        private async void OnVelocidadSeleccionada(
            object sender,
            EventArgs e)
        {
            if (ignorarCambioSeleccion)
                return;

            int indice = SelectorVelocidad.SelectedIndex;

            if (indice < 0 || indice >= VelocidadesMs.Length)
                return;

            int velocidad = VelocidadesMs[indice];

            string comando =
                PrefijoComandoVelocidad +
                velocidad.ToString(CultureInfo.InvariantCulture);

            MostrarEstado(
                LabelEstadoEnvio,
                "Envoi de " + comando + "...",
                ColorNeutro);

            bool enviado =
                await BluetoothManager.Instancia
                    .EnviarComandoAsync(comando);

            if (enviado)
            {
                MostrarEstado(
                    LabelEstadoEnvio,
                    "Envoyé au Portenta : " + comando,
                    ColorExito);

                return;
            }

            MostrarEstado(
                LabelEstadoEnvio,
                "Impossible d'envoyer " + comando + ". " +
                MotivoDeFallo(),
                ColorError);

            /*
             * Se limpia la selección para que el usuario pueda volver
             * a elegir la MISMA velocidad y reintentar: si se dejara
             * marcada, escogerla de nuevo no dispararía ningún evento.
             */
            RestablecerSeleccion();
        }

        private void RestablecerSeleccion()
        {
            ignorarCambioSeleccion = true;

            SelectorVelocidad.SelectedIndex = -1;

            ignorarCambioSeleccion = false;
        }

        // ============================================================
        // Interruptor de salida por puerto serie
        // ============================================================

        private async void OnInterruptorSerialPulsado(
            object sender,
            EventArgs e)
        {
            // Un segundo toque mientras se envía el primero no debe
            // encadenar dos comandos contradictorios.
            if (cambioSerialEnCurso)
                return;

            cambioSerialEnCurso = true;

            try
            {
                bool destino = !salidaSerialActiva;

                string comando =
                    PrefijoComandoSerial +
                    (destino ? "True" : "False");

                // Atenuar el control indica que está ocupado.
                ContenedorInterruptor.Opacity = 0.5;

                MostrarEstado(
                    LabelEstadoSerial,
                    "Envoi de " + comando + "...",
                    ColorNeutro);

                bool enviado =
                    await BluetoothManager.Instancia
                        .EnviarComandoAsync(comando);

                /*
                 * El interruptor solo se mueve si el comando llegó a
                 * enviarse. Moverlo antes daría a entender que el
                 * Portenta cambió de estado cuando quizá ni siquiera
                 * hay conexión.
                 */
                if (!enviado)
                {
                    MostrarEstado(
                        LabelEstadoSerial,
                        "Impossible d'envoyer " + comando + ". " +
                        MotivoDeFallo(),
                        ColorError);

                    return;
                }

                salidaSerialActiva = destino;

                await AnimarInterruptorAsync(salidaSerialActiva);

                MostrarEstado(
                    LabelEstadoSerial,
                    salidaSerialActiva
                        ? "Activée · " + comando
                        : "Désactivée · " + comando,
                    salidaSerialActiva ? ColorExito : ColorNeutro);
            }
            finally
            {
                ContenedorInterruptor.Opacity = 1;

                cambioSerialEnCurso = false;
            }
        }

        // Coloca el interruptor sin animación (estado inicial).
        private void ColocarInterruptor(bool activo)
        {
            PistaInterruptor.BackgroundColor =
                Tema.Color(ClaveColorPista(activo));

            PulgarInterruptor.TranslationX =
                activo ? RecorridoPulgar : 0;
        }

        private async Task AnimarInterruptorAsync(bool activo)
        {
            PistaInterruptor.BackgroundColor =
                Tema.Color(ClaveColorPista(activo));

            await PulgarInterruptor.TranslateTo(
                activo ? RecorridoPulgar : 0,
                0,
                DuracionAnimacionMs,
                Easing.CubicOut);
        }

        private string ClaveColorPista(bool activo)
        {
            return activo
                ? "ColorAcento"
                : "ColorIndicadorInactivo";
        }

        // ============================================================
        // Reloj del Portenta
        // ============================================================

        /*
         * La hora se manda sola al abrir la página. Es el momento en
         * que consta que la app está viva y hay conexión, y evita
         * depender de que el usuario se acuerde de pulsar el botón.
         *
         * El botón queda como refuerzo, para reintentar cuando en ese
         * momento no hubiera conexión.
         */
        protected override async void OnAppearing()
        {
            base.OnAppearing();

            MostrarHoraDelTelefono();

            if (!BluetoothManager.Instancia.EstaConectado)
            {
                MostrarEstado(
                    LabelEstadoHora,
                    "Aucune connexion avec le PortentaH7. " +
                    "Utilisez le bouton une fois connecté.",
                    ColorNeutro);

                return;
            }

            await SincronizarHoraAsync(true);
        }

        private async void OnSincronizarHoraPulsado(
            object sender,
            EventArgs e)
        {
            MostrarHoraDelTelefono();

            await SincronizarHoraAsync(false);
        }

        private async Task SincronizarHoraAsync(bool automatica)
        {
            if (sincronizacionHoraEnCurso)
                return;

            sincronizacionHoraEnCurso = true;

            try
            {
                BotonSincronizarHora.IsEnabled = false;

                /*
                 * Se captura el instante una sola vez y se reutiliza
                 * para el comando y para el mensaje, de modo que lo
                 * que se muestra sea exactamente lo que se envió.
                 */
                DateTime ahora = DateTime.Now;

                string horaTexto =
                    ahora.ToString(
                        FormatoHora,
                        CultureInfo.InvariantCulture);

                string comando = PrefijoComandoHora + horaTexto;

                MostrarEstado(
                    LabelEstadoHora,
                    "Envoi de " + comando + "...",
                    ColorNeutro);

                /*
                 * Por la vía larga: el comando ocupa 27 bytes y la
                 * característica RX admite 20 por escritura.
                 */
                bool enviado =
                    await BluetoothManager.Instancia
                        .EnviarComandoLargoAsync(comando);

                if (enviado)
                {
                    MostrarEstado(
                        LabelEstadoHora,
                        (automatica
                            ? "Horloge synchronisée automatiquement : "
                            : "Horloge mise à l'heure : ") +
                        horaTexto,
                        ColorExito);

                    return;
                }

                MostrarEstado(
                    LabelEstadoHora,
                    "Impossible d'envoyer " + comando + ". " +
                    MotivoDeFallo(),
                    ColorError);
            }
            finally
            {
                BotonSincronizarHora.IsEnabled = true;

                sincronizacionHoraEnCurso = false;
            }
        }

        private void MostrarHoraDelTelefono()
        {
            LabelHoraTelefono.Text =
                "Heure du téléphone : " +
                DateTime.Now.ToString(
                    FormatoHora,
                    CultureInfo.InvariantCulture);
        }

        // ============================================================
        // Utilidades comunes
        // ============================================================

        private string MotivoDeFallo()
        {
            return BluetoothManager.Instancia.EstaConectado
                ? "Aucune caractéristique BLE d'écriture trouvée, ou l'envoi a échoué."
                : "Aucune connexion avec le PortentaH7.";
        }

        private void MostrarEstado(
            Label destino,
            string mensaje,
            string claveColor)
        {
            destino.Text = mensaje;

            destino.TextColor = Tema.Color(claveColor);
        }
    }
}
