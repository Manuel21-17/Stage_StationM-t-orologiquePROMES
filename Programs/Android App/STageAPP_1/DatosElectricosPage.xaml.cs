using System;
using System.Collections.Generic;
using System.Globalization;
using Xamarin.Forms;

namespace STageAPP_1
{
    public partial class DatosElectricosPage : ContentPage
    {
        private const int NumeroDeGraficas =
            ConfiguracionSensores.NumeroDeEntradas;

        private const int MaximoDeMuestras = 30;

        /*
         * Los datos entran al ritmo del Portenta (hasta 200 paquetes
         * por segundo con comvel:5), pero la pantalla se refresca a
         * este ritmo fijo.
         *
         * Sin esta separación, cada paquete obligaba a repintar las 23
         * gráficas: a 500 ms ya se notaba, y a 5 ms sería imposible.
         */
        private const int IntervaloRefrescoMs = 120;

        /*
         * La escala vertical depende del TIPO de sensor, porque el
         * Portenta ya manda el valor calibrado y cada recta llega a un
         * tope distinto. Los topes viven en ConfiguracionSensores, que
         * es donde se documenta su relación con el firmware.
         *
         * Es fija a propósito. Si se autoajustara, el ruido de una
         * entrada en reposo llenaría la pantalla y parecería una señal.
         */
        private const float MinimoEscala = 0;

        private readonly Dictionary<int, VistaGrafica> graficas =
            new Dictionary<int, VistaGrafica>();

        private readonly Dictionary<int, Label> etiquetasValor =
            new Dictionary<int, Label>();

        // Sous-titre de chaque carte. On le garde pour pouvoir le
        // réécrire quand la configuration change.
        private readonly Dictionary<int, Label> etiquetasTipo =
            new Dictionary<int, Label>();

        private readonly Dictionary<int, List<float>> historialDatos =
            new Dictionary<int, List<float>>();

        private readonly Dictionary<int, TipoSensor> tiposMedicion =
            new Dictionary<int, TipoSensor>();

        private bool eventoBleSuscrito;

        // Hay muestras nuevas sin pintar.
        private bool refrescoPendiente;

        private bool temporizadorActivo;

        public DatosElectricosPage()
        {
            InitializeComponent();
            CrearGraficasElectricas();
        }

        protected override void OnAppearing()
        {
            base.OnAppearing();

            AplicarConfiguracionSensores();

            if (!eventoBleSuscrito)
            {
                BluetoothManager.Instancia.DatoRecibido +=
                    OnDatoBleRecibido;

                eventoBleSuscrito = true;
            }

            if (!temporizadorActivo)
            {
                temporizadorActivo = true;

                Device.StartTimer(
                    TimeSpan.FromMilliseconds(IntervaloRefrescoMs),
                    OnTemporizadorRefresco);
            }
        }

        protected override void OnDisappearing()
        {
            base.OnDisappearing();

            // Devolver false en el callback detiene el temporizador.
            temporizadorActivo = false;

            if (eventoBleSuscrito)
            {
                BluetoothManager.Instancia.DatoRecibido -=
                    OnDatoBleRecibido;

                eventoBleSuscrito = false;
            }
        }

        private bool OnTemporizadorRefresco()
        {
            if (!temporizadorActivo)
                return false;

            if (refrescoPendiente)
            {
                refrescoPendiente = false;

                for (int numeroGrafica = 1;
                     numeroGrafica <= NumeroDeGraficas;
                     numeroGrafica++)
                {
                    RefrescarGrafica(numeroGrafica);
                }
            }

            return true;
        }

        private void CrearGraficasElectricas()
        {
            ContenedorGraficas.Children.Clear();

            for (int numeroGrafica = 1;
                 numeroGrafica <= NumeroDeGraficas;
                 numeroGrafica++)
            {
                CrearTarjetaGrafica(
                    numeroGrafica,
                    ConfiguracionSensores.Obtener(numeroGrafica));
            }
        }

        private void AplicarConfiguracionSensores()
        {
            for (int numeroGrafica = 1;
                 numeroGrafica <= NumeroDeGraficas;
                 numeroGrafica++)
            {
                CambiarTipoDeMedicion(
                    numeroGrafica,
                    ConfiguracionSensores.Obtener(numeroGrafica));
            }
        }

        private void CrearTarjetaGrafica(
            int numeroGrafica,
            TipoSensor tipoMedicion)
        {
            tiposMedicion[numeroGrafica] = tipoMedicion;
            historialDatos[numeroGrafica] = new List<float>();

            Label etiquetaEntrada = new Label
            {
                Text = ConfiguracionSensores
                    .NombreEntrada(numeroGrafica),
                FontSize = 20,
                FontAttributes = FontAttributes.Bold,
                TextColor = Tema.Color("ColorTextoPrincipal")
            };

            Label etiquetaTipoGrafica = new Label
            {
                Text = TextoSubtitulo(tipoMedicion),
                FontSize = 14,
                TextColor = Tema.Color("ColorTextoSecundario")
            };

            Label etiquetaValorActual = new Label
            {
                Text = ObtenerTextoValor(0, tipoMedicion),
                FontSize = 27,
                FontAttributes = FontAttributes.Bold,
                TextColor = ObtenerColorXamarin(tipoMedicion),
                HorizontalTextAlignment = TextAlignment.End,
                VerticalTextAlignment = TextAlignment.Center
            };

            VistaGrafica grafica = new VistaGrafica
            {
                HeightRequest = 230,
                HorizontalOptions = LayoutOptions.FillAndExpand,
                Opciones = CrearOpciones(tipoMedicion),
                Paleta = CrearPaleta()
            };

            grafica.Mostrar(null);

            Grid encabezado = new Grid
            {
                ColumnSpacing = 10
            };

            encabezado.ColumnDefinitions.Add(
                new ColumnDefinition
                {
                    Width = GridLength.Star
                });

            encabezado.ColumnDefinitions.Add(
                new ColumnDefinition
                {
                    Width = GridLength.Auto
                });

            StackLayout textosEncabezado = new StackLayout
            {
                Spacing = 2,
                Children =
                {
                    etiquetaEntrada,
                    etiquetaTipoGrafica
                }
            };

            encabezado.Children.Add(textosEncabezado, 0, 0);
            encabezado.Children.Add(etiquetaValorActual, 1, 0);

            Frame tarjeta = new Frame
            {
                BackgroundColor = Tema.Color("ColorTarjeta"),
                BorderColor = Tema.Color("ColorBordeTarjeta"),
                CornerRadius = 15,
                HasShadow = false,
                Padding = new Thickness(16),
                Content = new StackLayout
                {
                    Spacing = 12,
                    Children =
                    {
                        encabezado,
                        grafica
                    }
                }
            };

            graficas[numeroGrafica] = grafica;
            etiquetasValor[numeroGrafica] = etiquetaValorActual;
            etiquetasTipo[numeroGrafica] = etiquetaTipoGrafica;

            ContenedorGraficas.Children.Add(tarjeta);
        }

        private OpcionesGrafica CrearOpciones(TipoSensor tipoMedicion)
        {
            return new OpcionesGrafica
            {
                MinimoY = MinimoEscala,
                MaximoY = ConfiguracionSensores
                    .MaximoEscala(tipoMedicion),
                DivisionesY = ConfiguracionSensores
                    .DivisionesEscala(tipoMedicion),
                DivisionesX = 6,
                FormatoY = "0",
                MuestrasVisibles = MaximoDeMuestras,
                ColorSerie = ObtenerColorGrafica(tipoMedicion),
                TextoSinDatos = "Aucune donnée"
            };
        }

        private PaletaGrafica CrearPaleta()
        {
            return new PaletaGrafica
            {
                Fondo = Tema.ColorSK("ColorTarjeta"),
                Rejilla = Tema.ColorSK("ColorRejillaGrafica"),
                Ejes = Tema.ColorSK("ColorEjesGrafica"),
                Texto = Tema.ColorSK("ColorTextoSecundario")
            };
        }

        private void OnDatoBleRecibido(
            object sender,
            DatoBleEventArgs e)
        {
            if (e == null || string.IsNullOrWhiteSpace(e.Texto))
                return;

            ProcesarPaqueteAdc(e.Texto);
        }

        private void ProcesarPaqueteAdc(string paquete)
        {
            paquete = paquete.Trim();

            if (!paquete.StartsWith(
                    "$ADC",
                    StringComparison.OrdinalIgnoreCase))
            {
                return;
            }

            Dictionary<int, float> valoresRecibidos =
                new Dictionary<int, float>();

            string[] partes = paquete.Split(',');

            for (int i = 1; i < partes.Length; i++)
            {
                string[] canalYValor =
                    partes[i].Split(':');

                if (canalYValor.Length != 2)
                    continue;

                string nombreCanal =
                    canalYValor[0].Trim();

                string textoValor =
                    canalYValor[1].Trim();

                if (!nombreCanal.StartsWith(
                        "A",
                        StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                string textoNumeroCanal =
                    nombreCanal.Substring(1);

                if (!int.TryParse(
                        textoNumeroCanal,
                        out int numeroCanal))
                {
                    continue;
                }

                if (numeroCanal < 0 ||
                    numeroCanal >= NumeroDeGraficas)
                {
                    continue;
                }

                if (!float.TryParse(
                        textoValor,
                        NumberStyles.Float,
                        CultureInfo.InvariantCulture,
                        out float valor))
                {
                    continue;
                }

                valoresRecibidos[numeroCanal] = valor;
            }

            /*
             * A0 corresponde a la gráfica 1, A1 a la 2, y así hasta
             * A22 con la 23. Los canales ausentes se actualizan
             * temporalmente con cero.
             */
            for (int numeroCanal = 0;
                 numeroCanal < NumeroDeGraficas;
                 numeroCanal++)
            {
                float valor = 0;

                if (valoresRecibidos.ContainsKey(numeroCanal))
                {
                    valor = valoresRecibidos[numeroCanal];
                }

                RegistrarMuestra(numeroCanal + 1, valor);
            }

            // El repintado lo hace el temporizador, no cada paquete.
            refrescoPendiente = true;
        }

        private void RegistrarMuestra(
            int numeroEntrada,
            float nuevoValor)
        {
            if (!historialDatos.ContainsKey(numeroEntrada))
                return;

            List<float> datos =
                historialDatos[numeroEntrada];

            // Ventana deslizante: la gráfica se desplaza en lugar de
            // vaciarse cada vez que se llena.
            if (datos.Count >= MaximoDeMuestras)
            {
                datos.RemoveAt(0);
            }

            datos.Add(nuevoValor);
        }

        private void RefrescarGrafica(int numeroEntrada)
        {
            if (!historialDatos.ContainsKey(numeroEntrada) ||
                !graficas.ContainsKey(numeroEntrada) ||
                !etiquetasValor.ContainsKey(numeroEntrada))
            {
                return;
            }

            List<float> datos =
                historialDatos[numeroEntrada];

            TipoSensor tipoMedicion =
                tiposMedicion[numeroEntrada];

            float ultimoValor =
                datos.Count > 0
                    ? datos[datos.Count - 1]
                    : 0;

            etiquetasValor[numeroEntrada].Text =
                ObtenerTextoValor(ultimoValor, tipoMedicion);

            graficas[numeroEntrada].Mostrar(datos);
        }

        // Registra una muestra y la pinta de inmediato.
        public void ActualizarEntradaElectrica(
            int numeroEntrada,
            float nuevoValor)
        {
            RegistrarMuestra(numeroEntrada, nuevoValor);

            RefrescarGrafica(numeroEntrada);
        }

        private string TextoSubtitulo(TipoSensor tipoMedicion)
        {
            if (tipoMedicion == TipoSensor.SinAsignar)
            {
                return ConfiguracionSensores.Descripcion(tipoMedicion);
            }

            return ConfiguracionSensores.Descripcion(tipoMedicion) +
                   " en fonction du temps";
        }

        private string ObtenerTextoValor(
            float valor,
            TipoSensor tipoMedicion)
        {
            if (tipoMedicion == TipoSensor.SinAsignar)
            {
                // Sin configurar, el Portenta manda la cuenta cruda del
                // ADC: sin decimales y sin unidad, que sería inventada.
                return valor.ToString("0");
            }

            // El resto ya llega calibrado por el Portenta. El
            // meteorológico no lleva unidad porque depende del sensor
            // concreto que haya conectado.
            return valor.ToString("0.00") +
                   ConfiguracionSensores.Unidad(tipoMedicion);
        }

        private SkiaSharp.SKColor ObtenerColorGrafica(
            TipoSensor tipoMedicion)
        {
            return Tema.ColorSK(
                ConfiguracionSensores.ClaveColor(tipoMedicion));
        }

        private Color ObtenerColorXamarin(
            TipoSensor tipoMedicion)
        {
            return Tema.Color(
                ConfiguracionSensores.ClaveColor(tipoMedicion));
        }

        public void CambiarTipoDeMedicion(
            int numeroEntrada,
            TipoSensor nuevoTipo)
        {
            if (!tiposMedicion.ContainsKey(numeroEntrada))
                return;

            if (tiposMedicion[numeroEntrada] == nuevoTipo)
                return;

            tiposMedicion[numeroEntrada] = nuevoTipo;

            /*
             * El historial se descarta: las muestras anteriores son de
             * otra magnitud y mezclarlas en la misma serie daría una
             * gráfica sin sentido.
             */
            historialDatos[numeroEntrada].Clear();

            etiquetasTipo[numeroEntrada].Text =
                TextoSubtitulo(nuevoTipo);

            etiquetasValor[numeroEntrada].Text =
                ObtenerTextoValor(0, nuevoTipo);

            etiquetasValor[numeroEntrada].TextColor =
                ObtenerColorXamarin(nuevoTipo);

            graficas[numeroEntrada].Opciones =
                CrearOpciones(nuevoTipo);

            graficas[numeroEntrada].Mostrar(
                historialDatos[numeroEntrada]);
        }
    }
}
