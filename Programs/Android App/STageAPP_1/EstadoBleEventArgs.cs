using System;

namespace STageAPP_1
{
    public class EstadoBleEventArgs : EventArgs
    {
        public EstadoConexionBle Estado { get; }

        public string Mensaje { get; }

        public string NombreDispositivo { get; }

        public EstadoBleEventArgs(
            EstadoConexionBle estado,
            string mensaje,
            string nombreDispositivo = "")
        {
            Estado = estado;
            Mensaje = mensaje;
            NombreDispositivo = nombreDispositivo;
        }
    }
}