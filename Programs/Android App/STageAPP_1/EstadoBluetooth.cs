using System;

namespace STageAPP_1
{
    public static class EstadoBluetooth
    {
        public static bool EstaConectado { get; private set; }

        public static bool EstaConectando { get; private set; }

        public static string NombreDispositivo { get; private set; }
            = "Ningún dispositivo conectado";

        public static event EventHandler EstadoCambiado;

        public static void MarcarConectando(string nombreDispositivo)
        {
            EstaConectando = true;
            EstaConectado = false;

            NombreDispositivo =
                string.IsNullOrWhiteSpace(nombreDispositivo)
                ? "Dispositivo Bluetooth"
                : nombreDispositivo;

            NotificarCambio();
        }

        public static void MarcarConectado(string nombreDispositivo)
        {
            EstaConectando = false;
            EstaConectado = true;

            NombreDispositivo =
                string.IsNullOrWhiteSpace(nombreDispositivo)
                ? "Dispositivo Bluetooth"
                : nombreDispositivo;

            NotificarCambio();
        }

        public static void MarcarDesconectado()
        {
            EstaConectando = false;
            EstaConectado = false;
            NombreDispositivo = "Ningún dispositivo conectado";

            NotificarCambio();
        }

        private static void NotificarCambio()
        {
            EstadoCambiado?.Invoke(null, EventArgs.Empty);
        }
    }
}