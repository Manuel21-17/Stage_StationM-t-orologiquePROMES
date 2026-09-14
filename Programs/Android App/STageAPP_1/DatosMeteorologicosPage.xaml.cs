using System;
using System.Collections.Generic;
using System.Globalization;
using SkiaSharp;
using Xamarin.Forms;

namespace STageAPP_1
{
    public partial class DatosMeteorologicosPage : ContentPage
    {
        /*
         * =====================================================
         * DEUX ORIGINES DE DONNÉES, DEUX TYPES DE LIGNE
         * =====================================================
         *
         * $MET  -> station Davis. Captée par le Heltec en radio,
         *          transmise au Portenta par SPI, puis réémise en BLE.
         *          Arrive toutes les ~2,5 s. C'est la source RÉELLE de
         *          cet écran.
         *
         *          $MET,T:26.5,H:57.4,W:0.0,G:0.0,D:350,
         *               U:0.52,S:84.4,R:0.0,B:8.52,Q:-52.0
         *
         * $ADC  -> voies analogiques du Portenta. Elles alimentent les
         *          graphiques ÉLECTRIQUES, gérés sur leur propre écran.
         *          Leurs champs H/P/V/D n'apparaissent que s'ils sont
         *          injectés à la main avec la commande comchain:, et on
         *          continue de les accepter pour ne pas perdre ce
         *          chemin de test.
         *
         * Lec:  -> voies analogiques d'un Arduino Mega 2560. Le chemin
         *          est long : I2C jusqu'au heltec_puente, ESP-NOW
         *          jusqu'au Heltec du Davis, SPI jusqu'au Portenta, et
         *          enfin BLE jusqu'ici.
         *
         *          Lec:A0:23,A1:24,A2:812,A3:511,...,A15:0
         *
         *          LE MEGA ENVOIE DÉJÀ CALIBRÉ, il n'y a rien à
         *          convertir ici : A0 et A1 en °C (sonde Ta-ext-V-4090)
         *          et A2 en W/m² (cellule Si-I-420T). A3..A15 sont des
         *          comptes ADC bruts, sans emploi sur cet écran.
         *
         *          A2 vaut -999 quand la boucle 4-20 mA est ouverte.
         *          C'est une absence de mesure, pas une mesure : la
         *          tracer écraserait l'échelle du graphique.
         */
        private const string CabeceraPaquete = "$ADC";
        private const string CabeceraMeteo = "$MET";
        private const string CabeceraLec = "Lec:";

        // --- Clés du paquet $MET (station Davis) ---
        private const string ClaveMetTemperatura = "T";
        private const string ClaveMetHumedad = "H";
        private const string ClaveMetViento = "W";
        private const string ClaveMetRacha = "G";
        private const string ClaveMetDireccion = "D";
        private const string ClaveMetUV = "U";
        private const string ClaveMetSolar = "S";
        private const string ClaveMetLluvia = "R";
        private const string ClaveMetRssi = "Q";

        // --- Clés du paquet $ADC (injection manuelle avec comchain:) ---
        private const string ClaveHumedad = "H";
        private const string ClaveCeldaReferencia1 = "P";
        private const string ClaveVelocidadViento = "V";
        private const string ClaveDireccionViento = "D";

        // --- Voies du paquet Lec: (Arduino Mega) ---
        // Correspondance décidée par l'utilisateur.
        private const string ClaveLecTemperatura3 = "A0";
        private const string ClaveLecTemperaturaInterna = "A1";
        private const string ClaveLecCeldaReferencia2 = "A2";

        /*
         * Clés internes du dictionnaire de séries.
         *
         * Préfixées à dessein : le paquet $ADC du Portenta transporte lui
         * aussi des voies nommées A0..A7, qui n'ont rien à voir avec
         * celles du Mega. Sans ce préfixe, les deux sources écriraient
         * dans les mêmes courbes.
         */
        private const string SerieLecTemperatura3 = "LEC_A0";
        private const string SerieLecTemperaturaInterna = "LEC_A1";
        private const string SerieLecCeldaReferencia2 = "LEC_A2";

        // Ce que publie le Mega quand la boucle 4-20 mA est ouverte.
        private const double ValorInvalidoMega = -999.0;

        private const string SinDato = "--";
        private const string UnidadIrradiancia = " W/m²";
        private const string TextoSinDatos = "Aucune donnée";

        private const int MaximoMuestrasGrafica = 30;

        private bool eventoBleSuscrito;

        private sealed class SerieMeteo
        {
            public VistaGrafica Vista;
            public readonly List<float> Historial = new List<float>();
        }

        private readonly Dictionary<string, SerieMeteo> series =
            new Dictionary<string, SerieMeteo>();

        public DatosMeteorologicosPage()
        {
            InitializeComponent();

            ConfigurarGraficas();

            MostrarSinDatos();
        }

        /*
         * Cada gráfica lleva su escala vertical FIJA, elegida para la
         * magnitud que representa. No se autoajusta con los datos: una
         * escala que se mueve con cada muestra impide comparar de un
         * vistazo y hace que el ruido parezca una variación enorme.
         */
        private void ConfigurarGraficas()
        {
            // Escala pedida explícitamente: -5 a 50 °C, de 5 en 5.
            RegistrarSerie(ClaveMetTemperatura, GraficaTemperatura,
                -5, 50, 11, "0", "ColorTemperatura");

            RegistrarSerie(ClaveMetHumedad, GraficaHumedad,
                0, 100, 4, "0", "ColorHumedad");

            RegistrarSerie(ClaveMetViento, GraficaViento,
                0, 60, 6, "0", "ColorViento");

            RegistrarSerie(ClaveMetRacha, GraficaRacha,
                0, 120, 6, "0", "ColorRacha");

            RegistrarSerie(ClaveMetSolar, GraficaRadiacionSolar,
                0, 1200, 6, "0", "ColorRadiacionSolar");

            RegistrarSerie(ClaveMetUV, GraficaUV,
                0, 12, 6, "0", "ColorUV");

            // En Grafana este panel se dibuja con barras.
            RegistrarSerie(ClaveMetLluvia, GraficaLluvia,
                0, 10, 5, "0", "ColorLluvia", barras: true);

            // Escala del panel de Grafana: por debajo de -100 dBm se
            // empiezan a perder tramas.
            RegistrarSerie(ClaveMetRssi, GraficaRssi,
                -110, -20, 6, "0", "ColorRssi");

            /*
             * Ces trois-là ne viennent PAS de la Davis mais des voies
             * analogiques du Mega, par le paquet Lec:. Aucun conflit
             * avec la station : elle n'a jamais alimenté ces courbes.
             *
             * Les échelles restent celles déjà choisies ici.
             */
            RegistrarSerie(SerieLecTemperaturaInterna,
                GraficaTemperaturaInterna,
                -5, 50, 11, "0", "ColorTemperatura");

            RegistrarSerie(SerieLecTemperatura3,
                GraficaTemperatura3,
                -5, 50, 11, "0", "ColorTemperatura");

            RegistrarSerie(SerieLecCeldaReferencia2,
                GraficaCeldaReferencia2,
                0, 1200, 6, "0", "ColorRadiacionSolar");

            /*
             * Celle-ci reste sans série : la Davis entrega UNE seule
             * mesure d'irradiance, et personne n'alimente encore cette
             * deuxième cellule de référence. On lui met quand même une
             * échelle, pour afficher la grille au lieu d'un trou.
             */
            PrepararGraficaVacia(GraficaCeldaReferencia1,
                0, 1200, 6, "0", "ColorRadiacionSolar");
        }

        private void RegistrarSerie(
            string clave,
            VistaGrafica vista,
            float minimo,
            float maximo,
            int divisionesY,
            string formato,
            string claveColor,
            bool barras = false)
        {
            vista.Opciones = CrearOpciones(
                minimo, maximo, divisionesY, formato,
                claveColor, barras);

            vista.Paleta = CrearPaleta();

            series[clave] = new SerieMeteo { Vista = vista };

            vista.Mostrar(null);
        }

        private void PrepararGraficaVacia(
            VistaGrafica vista,
            float minimo,
            float maximo,
            int divisionesY,
            string formato,
            string claveColor)
        {
            vista.Opciones = CrearOpciones(
                minimo, maximo, divisionesY, formato,
                claveColor, false);

            vista.Paleta = CrearPaleta();

            vista.Mostrar(null);
        }

        private OpcionesGrafica CrearOpciones(
            float minimo,
            float maximo,
            int divisionesY,
            string formato,
            string claveColor,
            bool barras)
        {
            return new OpcionesGrafica
            {
                MinimoY = minimo,
                MaximoY = maximo,
                DivisionesY = divisionesY,
                DivisionesX = 6,
                FormatoY = formato,
                MuestrasVisibles = MaximoMuestrasGrafica,
                ColorSerie = Tema.ColorSK(claveColor),
                Barras = barras,
                TextoSinDatos = TextoSinDatos
            };
        }

        // Los colores salen de App.xaml, igual que el resto de la app.
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

        private void MostrarSinDatos()
        {
            LabelTemperatura.Text = SinDato + " °C";
            LabelHumedad.Text = SinDato + " %";
            LabelVelocidadViento.Text = SinDato + " km/h";

            LabelDireccionViento.Text =
                SinDato + " · " + SinDato + "°";

            FlechaDireccionViento.Rotation = 0;

            LabelTemperaturaInterna.Text = SinDato + " °C";
            LabelCeldaReferencia1.Text = SinDato + UnidadIrradiancia;
            LabelCeldaReferencia2.Text = SinDato + UnidadIrradiancia;
        }

        protected override void OnAppearing()
        {
            base.OnAppearing();

            if (!eventoBleSuscrito)
            {
                BluetoothManager.Instancia.DatoRecibido +=
                    OnDatoBleRecibido;

                eventoBleSuscrito = true;
            }
        }

        protected override void OnDisappearing()
        {
            base.OnDisappearing();

            if (eventoBleSuscrito)
            {
                BluetoothManager.Instancia.DatoRecibido -=
                    OnDatoBleRecibido;

                eventoBleSuscrito = false;
            }
        }

        private void OnDatoBleRecibido(
            object sender,
            DatoBleEventArgs e)
        {
            if (e == null || string.IsNullOrWhiteSpace(e.Texto))
                return;

            string paquete = e.Texto.Trim();

            if (paquete.StartsWith(
                    CabeceraMeteo,
                    StringComparison.OrdinalIgnoreCase))
            {
                ProcesarPaqueteMet(paquete);
                return;
            }

            if (paquete.StartsWith(
                    CabeceraLec,
                    StringComparison.OrdinalIgnoreCase))
            {
                ProcesarPaqueteLec(paquete);
                return;
            }

            if (paquete.StartsWith(
                    CabeceraPaquete,
                    StringComparison.OrdinalIgnoreCase))
            {
                ProcesarPaqueteMeteorologico(paquete);
            }
        }


        // ============================================================
        // Lec: — voies analogiques de l'Arduino Mega
        // ============================================================

        private void ProcesarPaqueteLec(string paquete)
        {
            string cuerpo = paquete.Substring(CabeceraLec.Length);

            string[] partes = cuerpo.Split(',');

            foreach (string parte in partes)
            {
                string[] claveYValor = parte.Split(':');

                if (claveYValor.Length != 2)
                    continue;

                string clave =
                    claveYValor[0].Trim().ToUpperInvariant();

                if (!double.TryParse(
                        claveYValor[1].Trim(),
                        NumberStyles.Float,
                        CultureInfo.InvariantCulture,
                        out double valor))
                {
                    continue;
                }

                switch (clave)
                {
                    case ClaveLecTemperaturaInterna:

                        ActualizarTemperaturaInterna(valor);

                        AnadirMuestraDeSerie(
                            SerieLecTemperaturaInterna, valor);
                        break;

                    case ClaveLecTemperatura3:

                        // Cette courbe n'a pas d'étiquette associée dans
                        // la vue : seulement le graphique.
                        AnadirMuestraDeSerie(
                            SerieLecTemperatura3, valor);
                        break;

                    case ClaveLecCeldaReferencia2:

                        if (valor <= ValorInvalidoMega)
                        {
                            // Boucle ouverte : on affiche l'absence de
                            // mesure et on ne trace rien. Tracer -999
                            // écraserait l'échelle 0..1200.
                            LabelCeldaReferencia2.Text =
                                SinDato + UnidadIrradiancia;
                            break;
                        }

                        ActualizarCeldaReferencia2(valor);

                        AnadirMuestraDeSerie(
                            SerieLecCeldaReferencia2, valor);
                        break;
                }
            }
        }


        private void AnadirMuestraDeSerie(string clave, double valor)
        {
            if (series.TryGetValue(clave, out SerieMeteo serie))
            {
                AnadirMuestra(serie, (float)valor);
            }
        }

        // ============================================================
        // $MET
        // ============================================================

        private void ProcesarPaqueteMet(string paquete)
        {
            string[] partes = paquete.Split(',');

            foreach (string parte in partes)
            {
                string[] claveYValor = parte.Split(':');

                if (claveYValor.Length != 2)
                    continue;

                string clave =
                    claveYValor[0].Trim().ToUpperInvariant();

                if (!double.TryParse(
                        claveYValor[1].Trim(),
                        NumberStyles.Float,
                        CultureInfo.InvariantCulture,
                        out double valor))
                {
                    continue;
                }

                switch (clave)
                {
                    case ClaveMetTemperatura:

                        ActualizarTemperatura(valor);
                        break;

                    case ClaveMetHumedad:

                        ActualizarHumedad(valor);
                        break;

                    case ClaveMetViento:

                        ActualizarVelocidadViento(valor);
                        break;

                    case ClaveMetDireccion:

                        ActualizarDireccionViento(valor);
                        break;
                }

                if (series.TryGetValue(clave, out SerieMeteo serie))
                {
                    AnadirMuestra(serie, (float)valor);
                }
            }
        }

        // Ventana deslizante: al llegar la muestra que sobra se descarta
        // la más antigua y la gráfica se desplaza en lugar de vaciarse.
        private void AnadirMuestra(SerieMeteo serie, float valor)
        {
            if (serie.Historial.Count >= MaximoMuestrasGrafica)
            {
                serie.Historial.RemoveAt(0);
            }

            serie.Historial.Add(valor);

            serie.Vista.Mostrar(serie.Historial);
        }

        // ============================================================
        // $ADC — injection manuelle avec comchain:
        // ============================================================

        private void ProcesarPaqueteMeteorologico(string paquete)
        {
            string[] partes = paquete.Split(',');

            foreach (string parte in partes)
            {
                string[] claveYValor = parte.Split(':');

                if (claveYValor.Length != 2)
                    continue;

                string clave = claveYValor[0].Trim();

                if (!double.TryParse(
                        claveYValor[1].Trim(),
                        NumberStyles.Float,
                        CultureInfo.InvariantCulture,
                        out double valor))
                {
                    continue;
                }

                switch (clave.ToUpperInvariant())
                {
                    case ClaveHumedad:

                        ActualizarHumedad(valor);
                        break;

                    case ClaveCeldaReferencia1:

                        ActualizarCeldaReferencia1(valor);
                        break;

                    case ClaveVelocidadViento:

                        ActualizarVelocidadViento(valor);
                        break;

                    case ClaveDireccionViento:

                        ActualizarDireccionViento(valor);
                        break;
                }
            }
        }

        // ============================================================
        // Indicateurs
        // ============================================================

        private void ActualizarTemperatura(double temperatura)
        {
            LabelTemperatura.Text =
                temperatura.ToString("0.0") + " °C";
        }

        private void ActualizarHumedad(double humedad)
        {
            LabelHumedad.Text =
                humedad.ToString("0.0") + " %";
        }

        private void ActualizarCeldaReferencia1(double irradiancia)
        {
            LabelCeldaReferencia1.Text =
                irradiancia.ToString("0") + UnidadIrradiancia;
        }

        public void ActualizarTemperaturaInterna(
            double temperaturaInterna)
        {
            LabelTemperaturaInterna.Text =
                temperaturaInterna.ToString("0.0") + " °C";
        }

        public void ActualizarCeldaReferencia2(double irradiancia)
        {
            LabelCeldaReferencia2.Text =
                irradiancia.ToString("0") + UnidadIrradiancia;
        }

        private void ActualizarVelocidadViento(double velocidadViento)
        {
            LabelVelocidadViento.Text =
                velocidadViento.ToString("0.0") + " km/h";
        }

        private void ActualizarDireccionViento(double grados)
        {
            grados = grados % 360;

            if (grados < 0)
            {
                grados += 360;
            }

            FlechaDireccionViento.Rotation = grados;

            LabelDireccionViento.Text =
                ObtenerPuntoCardinal(grados) +
                " · " +
                grados.ToString("0") +
                "°";
        }

        private string ObtenerPuntoCardinal(double grados)
        {
            string[] direcciones =
            {
                "Nord",
                "Nord-Est",
                "Est",
                "Sud-Est",
                "Sud",
                "Sud-Ouest",
                "Ouest",
                "Nord-Ouest"
            };

            int indice =
                (int)Math.Round(grados / 45.0) % 8;

            return direcciones[indice];
        }
    }
}
