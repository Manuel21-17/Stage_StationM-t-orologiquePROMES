using System;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Plugin.BLE;
using Plugin.BLE.Abstractions.Contracts;
using Plugin.BLE.Abstractions.EventArgs;
using Xamarin.Forms;

namespace STageAPP_1
{
    public sealed class BluetoothManager
    {
        private const string NombreObjetivo = "PortentaH7";

        private static readonly Guid UuidServicio =
            Guid.Parse("19B10000-E8F2-537E-4F6C-D104768A1214");

        private static readonly Guid UuidCaracteristicaTx =
            Guid.Parse("19B10001-E8F2-537E-4F6C-D104768A1214");

        // Característica de comandos app -> Portenta. En el firmware
        // se declara como BLEWrite con un tamaño máximo de 20 bytes.
        private static readonly Guid UuidCaracteristicaRx =
            Guid.Parse("19B10002-E8F2-537E-4F6C-D104768A1214");

        // El firmware recorta a 20 bytes lo que reciba por RX, así que
        // un comando más largo llegaría mutilado. Es preferible no
        // enviarlo y avisar.
        //
        // Es público porque quien construye comandos largos necesita
        // este límite para trocearlos antes de pedir el envío.
        public const int MaximoBytesComando = 20;

        private static readonly Lazy<BluetoothManager> InstanciaLazy =
            new Lazy<BluetoothManager>(
                () => new BluetoothManager());

        public static BluetoothManager Instancia =>
            InstanciaLazy.Value;

        private readonly IBluetoothLE bluetooth;
        private readonly IAdapter adaptador;
        private readonly SemaphoreSlim bloqueoConexion;

        private IDevice dispositivo;
        private IService servicio;
        private ICharacteristic caracteristicaTx;

        // Característica usada para enviar comandos AL Portenta.
        // Se resuelve la primera vez que se envía algo y se conserva
        // mientras dure la conexión.
        private ICharacteristic caracteristicaEscritura;

        private TaskCompletionSource<IDevice> dispositivoEncontrado;

        private bool conexionEnProceso;
        private bool reconexionProgramada;
        private const int RetrasoReconexionMs = 5000;

        // ============================================================
        // Buffer de reensamblado de mensajes BLE fragmentados.
        // El firmware trocea cada mensaje en paquetes de 20 bytes y
        // añade un '\n' al final del mensaje COMPLETO (no de cada
        // fragmento). Aquí acumulamos fragmentos hasta encontrar ese
        // '\n' y solo entonces publicamos el mensaje completo.
        // ============================================================
        private readonly StringBuilder bufferRecepcion = new StringBuilder();
        private const int LimiteBufferBytes = 4096;

        public event EventHandler<EstadoBleEventArgs> EstadoCambiado;
        public event EventHandler<DatoBleEventArgs> DatoRecibido;

        public EstadoConexionBle EstadoActual { get; private set; }

        public string UltimoDatoRecibido { get; private set; } =
            "En attente d'informations...";

        public string NombreDispositivoConectado
        {
            get
            {
                return dispositivo?.Name ?? string.Empty;
            }
        }

        public bool EstaConectado
        {
            get
            {
                return dispositivo != null &&
                       dispositivo.State ==
                       Plugin.BLE.Abstractions.DeviceState.Connected;
            }
        }

        private BluetoothManager()
        {
            bluetooth = CrossBluetoothLE.Current;
            adaptador = bluetooth.Adapter;
            bloqueoConexion = new SemaphoreSlim(1, 1);

            adaptador.DeviceDiscovered += OnDispositivoDescubierto;
            adaptador.DeviceDisconnected += OnDispositivoDesconectado;
            adaptador.DeviceConnectionLost += OnConexionPerdida;

            EstadoActual = EstadoConexionBle.Desconectado;
        }

        public async Task IniciarAsync()
        {
            if (conexionEnProceso || EstaConectado)
                return;

            await bloqueoConexion.WaitAsync();

            try
            {
                if (conexionEnProceso || EstaConectado)
                    return;

                conexionEnProceso = true;

                CambiarEstado(
                    EstadoConexionBle.SolicitandoPermisos,
                    "Demande des autorisations Bluetooth...");

                var servicioPermisos =
                    DependencyService.Get<IBluetoothPermissionService>();

                if (servicioPermisos == null)
                {
                    CambiarEstado(
                        EstadoConexionBle.Error,
                        "Service d'autorisations introuvable.");
                    return;
                }

                bool permisosConcedidos =
                    await servicioPermisos.SolicitarPermisosAsync();

                if (!permisosConcedidos)
                {
                    CambiarEstado(
                        EstadoConexionBle.Error,
                        "Les autorisations Bluetooth ont été refusées.");
                    return;
                }

                if (!bluetooth.IsAvailable)
                {
                    CambiarEstado(
                        EstadoConexionBle.Error,
                        "Cet appareil ne prend pas en charge le Bluetooth LE.");
                    return;
                }

                if (!bluetooth.IsOn)
                {
                    CambiarEstado(
                        EstadoConexionBle.BluetoothApagado,
                        "Bluetooth désactivé. En attente pour réessayer...");

                    ProgramarReconexion();

                    return;
                }

                await BuscarYConectarAsync();
            }
            catch (Exception ex)
            {
                CambiarEstado(
                    EstadoConexionBle.Error,
                    "Erreur BLE : " + ex.Message +
                    ". Nouvelle tentative dans 5 secondes.");

                ProgramarReconexion();
            }
            finally
            {
                conexionEnProceso = false;
                bloqueoConexion.Release();
            }
        }

        private async Task BuscarYConectarAsync()
        {
            CambiarEstado(
                EstadoConexionBle.Buscando,
                "Recherche du PortentaH7...");

            dispositivoEncontrado =
                new TaskCompletionSource<IDevice>();

            adaptador.ScanTimeout = 10000;

            Task escaneo =
                adaptador.StartScanningForDevicesAsync();

            Task esperaDispositivo =
                dispositivoEncontrado.Task;

            Task completada =
                await Task.WhenAny(
                    esperaDispositivo,
                    escaneo);

            if (completada != esperaDispositivo)
            {
                CambiarEstado(
                    EstadoConexionBle.Desconectado,
                    "PortentaH7 introuvable. Nouvelle tentative dans 5 secondes.");

                ProgramarReconexion();

                return;
            }

            dispositivo =
                await dispositivoEncontrado.Task;

            if (adaptador.IsScanning)
                await adaptador.StopScanningForDevicesAsync();

            CambiarEstado(
                EstadoConexionBle.DispositivoEncontrado,
                "PortentaH7 détecté.",
                dispositivo.Name);

            CambiarEstado(
                EstadoConexionBle.Conectando,
                "Connexion au PortentaH7...",
                dispositivo.Name);

            using (var cancelacion =
                   new CancellationTokenSource(
                       TimeSpan.FromSeconds(15)))
            {
                await adaptador.ConnectToDeviceAsync(
                    dispositivo,
                    cancellationToken: cancelacion.Token);
            }

            await ConfigurarCaracteristicaAsync();
        }

        private async Task ConfigurarCaracteristicaAsync()
        {
            servicio =
                await dispositivo.GetServiceAsync(
                    UuidServicio);

            if (servicio == null)
            {
                CambiarEstado(
                    EstadoConexionBle.Error,
                    "Service BLE attendu introuvable.");

                await DesconectarAsync();
                return;
            }

            caracteristicaTx =
                await servicio.GetCharacteristicAsync(
                    UuidCaracteristicaTx);

            if (caracteristicaTx == null)
            {
                CambiarEstado(
                    EstadoConexionBle.Error,
                    "Caractéristique TX introuvable.");

                await DesconectarAsync();
                return;
            }

            if (!caracteristicaTx.CanUpdate)
            {
                CambiarEstado(
                    EstadoConexionBle.Error,
                    "La caractéristique TX ne gère pas les notifications.");

                await DesconectarAsync();
                return;
            }

            caracteristicaTx.ValueUpdated -= OnValorActualizado;
            caracteristicaTx.ValueUpdated += OnValorActualizado;

            await caracteristicaTx.StartUpdatesAsync();

            CambiarEstado(
                EstadoConexionBle.Conectado,
                "BLE connecté",
                dispositivo.Name);
        }

        private async void OnDispositivoDescubierto(
            object sender,
            DeviceEventArgs e)
        {
            if (e.Device == null)
                return;

            if (!string.Equals(
                    e.Device.Name,
                    NombreObjetivo,
                    StringComparison.OrdinalIgnoreCase))
            {
                return;
            }

            dispositivoEncontrado?
                .TrySetResult(e.Device);

            try
            {
                if (adaptador.IsScanning)
                    await adaptador.StopScanningForDevicesAsync();
            }
            catch
            {
                // El escaneo ya pudo haberse detenido.
            }
        }

        // ============================================================
        // Recepción de notificaciones BLE crudas.
        // Cada notificación puede traer solo un FRAGMENTO del mensaje
        // completo (el firmware limita cada paquete a 20 bytes), así
        // que aquí solo acumulamos texto en el buffer y delegamos la
        // extracción de mensajes completos a ExtraerMensajesCompletos.
        // ============================================================
        private void OnValorActualizado(
            object sender,
            CharacteristicUpdatedEventArgs e)
        {
            byte[] datos = e.Characteristic.Value;

            if (datos == null || datos.Length == 0)
                return;

            string fragmento;

            try
            {
                fragmento = Encoding.UTF8.GetString(datos);
            }
            catch
            {
                // Fragmento no interpretable como texto; se descarta
                // en vez de corromper el buffer de reensamblado.
                return;
            }

            bufferRecepcion.Append(fragmento);

            if (bufferRecepcion.Length > LimiteBufferBytes)
            {
                // Salvaguarda: si nunca aparece un '\n' (por ejemplo,
                // se perdió un fragmento a mitad de mensaje), no
                // dejamos crecer el buffer indefinidamente.
                bufferRecepcion.Clear();
                return;
            }

            ExtraerMensajesCompletos();
        }

        // ============================================================
        // Recorre el buffer acumulado y publica cada mensaje completo
        // que encuentre (delimitado por '\n'), conservando cualquier
        // resto incompleto para el próximo fragmento que llegue.
        // ============================================================
        private void ExtraerMensajesCompletos()
        {
            string contenido = bufferRecepcion.ToString();

            int indiceFin = contenido.IndexOf('\n');

            while (indiceFin >= 0)
            {
                string mensajeCompleto = contenido
                    .Substring(0, indiceFin)
                    .Trim('\0', '\r', ' ');

                PublicarMensaje(mensajeCompleto);

                contenido = contenido.Substring(indiceFin + 1);
                indiceFin = contenido.IndexOf('\n');
            }

            bufferRecepcion.Clear();
            bufferRecepcion.Append(contenido);
        }

        // ============================================================
        // Dispara el evento DatoRecibido con un mensaje ya completo
        // y reensamblado, en el hilo principal de la UI.
        // ============================================================
        private void PublicarMensaje(string texto)
        {
            if (string.IsNullOrWhiteSpace(texto))
                return;

            UltimoDatoRecibido = texto;

            byte[] bytesMensaje = Encoding.UTF8.GetBytes(texto);

            Device.BeginInvokeOnMainThread(() =>
            {
                DatoRecibido?.Invoke(
                    this,
                    new DatoBleEventArgs(texto, bytesMensaje));
            });
        }

        // ============================================================
        // Envío de comandos hacia el PortentaH7.
        //
        // Devuelve true solo si el comando llegó a escribirse en la
        // característica BLE. La página que llama decide qué mensaje
        // mostrar al usuario según ese resultado.
        // ============================================================
        public async Task<bool> EnviarComandoAsync(string comando)
        {
            if (string.IsNullOrWhiteSpace(comando))
                return false;

            if (!EstaConectado || servicio == null)
                return false;

            try
            {
                ICharacteristic destino =
                    await ObtenerCaracteristicaEscrituraAsync();

                if (destino == null)
                    return false;

                byte[] datos = Encoding.UTF8.GetBytes(comando);

                if (datos.Length > MaximoBytesComando)
                    return false;

                await destino.WriteAsync(datos);

                return true;
            }
            catch
            {
                // Un fallo de escritura no debe tumbar la app; se
                // informa al usuario mediante el valor de retorno.
                return false;
            }
        }

        // ============================================================
        // Envío de comandos que NO caben en una sola escritura.
        //
        // La característica RX del firmware admite 20 bytes por
        // escritura, pero "comhora:AAAA-MM-DD hh:mm:ss" ocupa 27. El
        // firmware acumula lo que le llega por RX hasta encontrar un
        // '\n', así que basta con partir el comando en trozos de 20
        // bytes y mandarlos seguidos: al otro lado se reensamblan.
        //
        // El '\n' final forma parte del comando. Sin él el firmware
        // esperaría a que venciera su tiempo límite antes de dar la
        // línea por terminada.
        //
        // Se deja aparte de EnviarComandoAsync a propósito: los
        // comandos cortos siguen yendo por el camino de siempre.
        // ============================================================
        public async Task<bool> EnviarComandoLargoAsync(string comando)
        {
            if (string.IsNullOrWhiteSpace(comando))
                return false;

            if (!EstaConectado || servicio == null)
                return false;

            try
            {
                ICharacteristic destino =
                    await ObtenerCaracteristicaEscrituraAsync();

                if (destino == null)
                    return false;

                // Los comandos son ASCII, así que trocear por bytes no
                // puede partir un carácter por la mitad.
                byte[] datos =
                    Encoding.UTF8.GetBytes(comando + "\n");

                for (int inicio = 0;
                     inicio < datos.Length;
                     inicio += MaximoBytesComando)
                {
                    int longitud =
                        Math.Min(
                            MaximoBytesComando,
                            datos.Length - inicio);

                    byte[] trozo = new byte[longitud];

                    Array.Copy(datos, inicio, trozo, 0, longitud);

                    await destino.WriteAsync(trozo);
                }

                return true;
            }
            catch
            {
                return false;
            }
        }

        // ============================================================
        // Localiza la característica RX del Portenta, por la que se le
        // envían los comandos.
        //
        // Se busca primero por su UUID conocido. El recorrido posterior
        // solo actúa como red de seguridad por si el firmware cambiara
        // de característica en el futuro.
        // ============================================================
        private async Task<ICharacteristic> ObtenerCaracteristicaEscrituraAsync()
        {
            if (caracteristicaEscritura != null)
                return caracteristicaEscritura;

            if (servicio == null)
                return null;

            ICharacteristic rx =
                await servicio.GetCharacteristicAsync(
                    UuidCaracteristicaRx);

            if (rx != null && rx.CanWrite)
            {
                caracteristicaEscritura = rx;
                return caracteristicaEscritura;
            }

            var caracteristicas =
                await servicio.GetCharacteristicsAsync();

            if (caracteristicas == null)
                return null;

            foreach (ICharacteristic caracteristica in caracteristicas)
            {
                if (caracteristica != null && caracteristica.CanWrite)
                {
                    caracteristicaEscritura = caracteristica;
                    return caracteristicaEscritura;
                }
            }

            return null;
        }

        private void OnDispositivoDesconectado(
            object sender,
            DeviceEventArgs e)
        {
            LimpiarConexion();

            CambiarEstado(
                EstadoConexionBle.Desconectado,
                "PortentaH7 déconnecté. Tentative de reconnexion...");

            ProgramarReconexion();
        }

        private void OnConexionPerdida(
            object sender,
            DeviceErrorEventArgs e)
        {
            LimpiarConexion();

            CambiarEstado(
                EstadoConexionBle.Desconectado,
                "Connexion perdue. Tentative de reconnexion...");

            ProgramarReconexion();
        }
        private async void ProgramarReconexion()
        {
            if (reconexionProgramada)
                return;

            if (EstaConectado)
                return;

            reconexionProgramada = true;

            try
            {
                for (int segundos = 5; segundos >= 1; segundos--)
                {
                    CambiarEstado(
                        EstadoConexionBle.Desconectado,
                        $"PortentaH7 introuvable.\nNouvelle tentative dans {segundos} secondes.");

                    await Task.Delay(1000);

                    if (EstaConectado)
                        return;
                }
            }
            finally
            {
                reconexionProgramada = false;
            }

            if (!EstaConectado)
                await IniciarAsync();
        }
        public async Task DesconectarAsync()
        {
            try
            {
                if (caracteristicaTx != null)
                {
                    caracteristicaTx.ValueUpdated -=
                        OnValorActualizado;

                    if (caracteristicaTx.CanUpdate)
                        await caracteristicaTx.StopUpdatesAsync();
                }

                if (dispositivo != null && EstaConectado)
                    await adaptador.DisconnectDeviceAsync(dispositivo);
            }
            catch
            {
                // Evita que una desconexión incompleta cierre la aplicación.
            }
            finally
            {
                LimpiarConexion();

                CambiarEstado(
                    EstadoConexionBle.Desconectado,
                    "Bluetooth déconnecté.");
            }
        }

        private void LimpiarConexion()
        {
            if (caracteristicaTx != null)
                caracteristicaTx.ValueUpdated -= OnValorActualizado;

            caracteristicaTx = null;
            caracteristicaEscritura = null;
            servicio = null;
            dispositivo = null;

            // También reiniciamos el buffer de reensamblado: un mensaje
            // a medio recibir de una conexión anterior no tiene sentido
            // combinarlo con fragmentos de una conexión nueva.
            bufferRecepcion.Clear();
        }

        private void CambiarEstado(
            EstadoConexionBle estado,
            string mensaje,
            string nombreDispositivo = "")
        {
            EstadoActual = estado;

            Device.BeginInvokeOnMainThread(() =>
            {
                EstadoCambiado?.Invoke(
                    this,
                    new EstadoBleEventArgs(
                        estado,
                        mensaje,
                        nombreDispositivo));
            });
        }
    }
}