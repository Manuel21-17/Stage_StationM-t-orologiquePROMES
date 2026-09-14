using System;
using Xamarin.Forms;

namespace STageAPP_1
{
    public partial class MainPage : ContentPage
    {
        private readonly BluetoothManager bluetoothManager;

        private bool conexionIniciada;

        public MainPage()
        {
            InitializeComponent();

            bluetoothManager = BluetoothManager.Instancia;
        }

        protected override async void OnAppearing()
        {
            base.OnAppearing();

            // Evita suscribir varias veces los mismos eventos.
            bluetoothManager.EstadoCambiado -=
                OnEstadoBluetoothCambiado;

            bluetoothManager.DatoRecibido -=
                OnDatoBluetoothRecibido;

            bluetoothManager.EstadoCambiado +=
                OnEstadoBluetoothCambiado;

            bluetoothManager.DatoRecibido +=
                OnDatoBluetoothRecibido;

            // Recupera el estado actual al regresar a esta pantalla.
            MostrarEstadoActual();

            // La búsqueda BLE se inicia automáticamente una sola vez.
            if (!conexionIniciada)
            {
                conexionIniciada = true;

                await bluetoothManager.IniciarAsync();
            }
        }

        protected override void OnDisappearing()
        {
            base.OnDisappearing();

            bluetoothManager.EstadoCambiado -=
                OnEstadoBluetoothCambiado;

            bluetoothManager.DatoRecibido -=
                OnDatoBluetoothRecibido;
        }

        private void OnEstadoBluetoothCambiado(
            object sender,
            EstadoBleEventArgs e)
        {
            Device.BeginInvokeOnMainThread(() =>
            {
                ActualizarIndicadorBluetooth(
                    e.Estado,
                    e.Mensaje,
                    e.NombreDispositivo);
            });
        }

        private void OnDatoBluetoothRecibido(
            object sender,
            DatoBleEventArgs e)
        {
            Device.BeginInvokeOnMainThread(() =>
            {
                LabelUltimoDatoBluetooth.Text = e.Texto;
            });
        }

        private void MostrarEstadoActual()
        {
            ActualizarIndicadorBluetooth(
                bluetoothManager.EstadoActual,
                ObtenerMensajeDelEstado(
                    bluetoothManager.EstadoActual),
                bluetoothManager.NombreDispositivoConectado);

            LabelUltimoDatoBluetooth.Text =
                bluetoothManager.UltimoDatoRecibido;
        }

        private void ActualizarIndicadorBluetooth(
            EstadoConexionBle estado,
            string mensaje,
            string nombreDispositivo)
        {
            LabelEstadoBluetooth.Text = mensaje;

            if (string.IsNullOrWhiteSpace(nombreDispositivo))
            {
                LabelNombreDispositivo.Text =
                    estado == EstadoConexionBle.Buscando ||
                    estado == EstadoConexionBle.Conectando ||
                    estado == EstadoConexionBle.DispositivoEncontrado
                        ? "PortentaH7"
                        : "Aucun appareil connecté";
            }
            else
            {
                LabelNombreDispositivo.Text =
                    nombreDispositivo;
            }

            switch (estado)
            {
                case EstadoConexionBle.Conectado:

                    IndicadorBluetooth.BackgroundColor =
                        Tema.Color("ColorIndicadorActivo");

                    break;

                case EstadoConexionBle.SolicitandoPermisos:
                case EstadoConexionBle.Buscando:
                case EstadoConexionBle.DispositivoEncontrado:
                case EstadoConexionBle.Conectando:

                    IndicadorBluetooth.BackgroundColor =
                        Tema.Color("ColorIndicadorProceso");

                    break;

                case EstadoConexionBle.Error:

                    IndicadorBluetooth.BackgroundColor =
                        Tema.Color("ColorIndicadorError");

                    break;

                case EstadoConexionBle.BluetoothApagado:
                default:

                    IndicadorBluetooth.BackgroundColor =
                        Tema.Color("ColorIndicadorInactivo");

                    break;
            }
        }

        private string ObtenerMensajeDelEstado(
            EstadoConexionBle estado)
        {
            switch (estado)
            {
                case EstadoConexionBle.SolicitandoPermisos:
                    return "Demande des autorisations Bluetooth...";

                case EstadoConexionBle.BluetoothApagado:
                    return "Bluetooth désactivé";

                case EstadoConexionBle.Buscando:
                    return "Recherche du PortentaH7...";

                case EstadoConexionBle.DispositivoEncontrado:
                    return "PortentaH7 détecté";

                case EstadoConexionBle.Conectando:
                    return "Connexion au PortentaH7...";

                case EstadoConexionBle.Conectado:
                    return "BLE connecté";

                case EstadoConexionBle.Error:
                    return "Erreur de connexion BLE";

                default:
                    return "Bluetooth déconnecté";
            }
        }

        private async void OnConfigurarSensoresClicked(
            object sender,
            EventArgs e)
        {
            await Navigation.PushAsync(
                new ConfigurarSensoresPage());
        }

        private async void OnConfiguracionPortentaClicked(
            object sender,
            EventArgs e)
        {
            await Navigation.PushAsync(
                new ConfiguracionPortentaPage());
        }

        private async void OnVisualizarDatosClicked(
            object sender,
            EventArgs e)
        {
            await Navigation.PushAsync(
                new VisualizarDatosPage());
        }
    }
}