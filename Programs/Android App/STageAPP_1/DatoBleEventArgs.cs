using System;

namespace STageAPP_1
{
    public class DatoBleEventArgs : EventArgs
    {
        public string Texto { get; }

        public byte[] Datos { get; }

        public DatoBleEventArgs(string texto, byte[] datos)
        {
            Texto = texto;
            Datos = datos;
        }
    }
}