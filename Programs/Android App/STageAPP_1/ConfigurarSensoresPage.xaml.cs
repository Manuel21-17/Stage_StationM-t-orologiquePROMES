using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Xamarin.Forms;

namespace STageAPP_1
{
    public partial class ConfigurarSensoresPage : ContentPage
    {
        // Claves de la paleta definida en App.xaml.
        private const string ColorNeutro = "ColorTextoSecundario";
        private const string ColorExito = "ColorIndicadorActivo";
        private const string ColorError = "ColorIndicadorError";

        /*
         * Pausa entre escrituras BLE consecutivas.
         *
         * procesarEntradaBLE del firmware atiende como mucho una
         * escritura por vuelta de loop(): si dos llegan demasiado
         * juntas, la segunda puede pisar a la primera antes de que se
         * lea. Espaciarlas deja margen para procesar cada una.
         */
        private const int PausaEntreEnviosMs = 40;

        private bool envioEnCurso;

        public ConfigurarSensoresPage()
        {
            InitializeComponent();

            CrearTituloSeccion("Entrées Portenta");

            for (int numeroEntrada = 1;
                 numeroEntrada <= ConfiguracionSensores.EntradasPortenta;
                 numeroEntrada++)
            {
                CrearSelectorSensor(numeroEntrada);
            }

            CrearTituloSeccion("Entrées analogiques");

            for (int numeroEntrada =
                     ConfiguracionSensores.EntradasPortenta + 1;
                 numeroEntrada <= ConfiguracionSensores.NumeroDeEntradas;
                 numeroEntrada++)
            {
                CrearSelectorSensor(numeroEntrada);
            }
        }

        private async void OnBotonAccionSensoresPulsado(
            object sender,
            EventArgs e)
        {
            if (envioEnCurso)
                return;

            envioEnCurso = true;
            BotonAccionSensores.IsEnabled = false;

            try
            {
                List<string> comandos =
                    ConfiguracionSensores
                        .ConstruirComandosConfiguracion();

                /*
                 * Si el usuario no ha asignado ninguna entrada no hay
                 * nada que comunicar: se avisa en vez de enviar un
                 * comando vacío que el Portenta tendría que descartar.
                 */
                if (comandos.Count == 0)
                {
                    MostrarEstado(
                        "Aucune entrée affectée. Rien n'a été envoyé.",
                        ColorNeutro);

                    return;
                }

                int asignadas =
                    ConfiguracionSensores.ContarAsignadas();

                for (int i = 0; i < comandos.Count; i++)
                {
                    MostrarEstado(
                        "Envoi " + (i + 1) +
                        " sur " + comandos.Count + "...",
                        ColorNeutro);

                    bool enviado =
                        await BluetoothManager.Instancia
                            .EnviarComandoAsync(comandos[i]);

                    if (!enviado)
                    {
                        /*
                         * Se corta al primer fallo. Seguir enviando
                         * dejaría al Portenta con una configuración a
                         * medias sin que el usuario supiera cuál.
                         */
                        MostrarEstado(
                            "Échec du message " + (i + 1) +
                            " sur " + comandos.Count + ". " +
                            MotivoDeFallo() +
                            " La configuration est restée incomplète.",
                            ColorError);

                        return;
                    }

                    if (i < comandos.Count - 1)
                        await Task.Delay(PausaEntreEnviosMs);
                }

                MostrarEstado(
                    "Configuration envoyée : " + asignadas +
                    (asignadas == 1 ? " entrée" : " entrées") +
                    " en " + comandos.Count +
                    (comandos.Count == 1 ? " message." : " messages."),
                    ColorExito);
            }
            finally
            {
                BotonAccionSensores.IsEnabled = true;
                envioEnCurso = false;
            }
        }

        private string MotivoDeFallo()
        {
            return BluetoothManager.Instancia.EstaConectado
                ? "Aucune caractéristique BLE d'écriture trouvée, ou l'envoi a échoué."
                : "Aucune connexion avec le PortentaH7.";
        }

        private void MostrarEstado(
            string mensaje,
            string claveColor)
        {
            LabelEstadoConfiguracion.Text = mensaje;

            LabelEstadoConfiguracion.TextColor =
                Tema.Color(claveColor);
        }

        private void CrearTituloSeccion(string titulo)
        {
            Label etiquetaTitulo = new Label
            {
                Text = titulo,
                FontSize = 21,
                FontAttributes = FontAttributes.Bold,
                TextColor = Tema.Color("ColorAcento"),
                Margin = new Thickness(0, 15, 0, 5)
            };

            ContenedorSensores.Children.Add(etiquetaTitulo);
        }

        private void CrearSelectorSensor(int numeroEntrada)
        {
            Label etiquetaEntrada = new Label
            {
                Text = ConfiguracionSensores
                    .NombreEntrada(numeroEntrada),
                FontSize = 17,
                FontAttributes = FontAttributes.Bold,
                TextColor = Tema.Color("ColorTextoPrincipal")
            };

            Picker selectorSensor = new Picker
            {
                Title = "Sélectionner le type de capteur",
                FontSize = 16,
                BackgroundColor = Tema.Color("ColorFondoPrincipal"),
                TextColor = Tema.Color("ColorTextoPrincipal"),
                TitleColor = Tema.Color("ColorTextoSecundario"),
                HorizontalOptions = LayoutOptions.FillAndExpand,
                AutomationId = "Entrada" + numeroEntrada
            };

            foreach (TipoSensor tipo in
                     ConfiguracionSensores.TiposSeleccionables)
            {
                selectorSensor.Items.Add(
                    ConfiguracionSensores
                        .DescripcionSeleccionable(tipo));
            }

            /*
             * Se restaura lo último que eligió el usuario ANTES de
             * suscribir el evento. Asignar SelectedIndex dispara
             * SelectedIndexChanged, y hacerlo en este orden evita que
             * la restauración se confunda con una selección nueva.
             */
            selectorSensor.SelectedIndex =
                ConfiguracionSensores.IndiceDeTipo(
                    ConfiguracionSensores.Obtener(numeroEntrada));

            selectorSensor.SelectedIndexChanged +=
                (remitente, argumentos) =>
                {
                    int indice = selectorSensor.SelectedIndex;

                    if (indice < 0 ||
                        indice >= ConfiguracionSensores
                            .TiposSeleccionables.Length)
                    {
                        return;
                    }

                    ConfiguracionSensores.Asignar(
                        numeroEntrada,
                        ConfiguracionSensores
                            .TiposSeleccionables[indice]);
                };

            Frame contenedorEntrada = new Frame
            {
                BackgroundColor = Tema.Color("ColorTarjeta"),
                BorderColor = Tema.Color("ColorBordeTarjeta"),
                CornerRadius = 8,
                Padding = new Thickness(15),
                HasShadow = false,
                Content = new StackLayout
                {
                    Spacing = 8,
                    Children =
                    {
                        etiquetaEntrada,
                        selectorSensor
                    }
                }
            };

            ContenedorSensores.Children.Add(contenedorEntrada);
        }
    }
}
