using SkiaSharp;
using Xamarin.Forms;

namespace STageAPP_1
{
    /*
     * Acceso a la paleta definida en App.xaml desde código C#.
     *
     * Las páginas que construyen su interfaz por código (las
     * gráficas eléctricas y los selectores de sensores) escribían
     * los colores a mano, así que el tema vivía en dos sitios y se
     * desincronizaba. Con esto App.xaml vuelve a ser el único lugar
     * donde se define un color.
     */
    public static class Tema
    {
        public static Color Color(string clave)
        {
            if (Application.Current != null &&
                Application.Current.Resources != null &&
                Application.Current.Resources.TryGetValue(
                    clave,
                    out object valor) &&
                valor is Color color)
            {
                return color;
            }

            /*
             * Si la clave no existe se devuelve un magenta chillón
             * en lugar de un color plausible: así un error de
             * escritura salta a la vista en la primera ejecución en
             * vez de pasar por un tono válido.
             */
            return Xamarin.Forms.Color.FromHex("#FF00FF");
        }

        public static SKColor ColorSK(string clave)
        {
            Color color = Color(clave);

            return new SKColor(
                (byte)(color.R * 255),
                (byte)(color.G * 255),
                (byte)(color.B * 255),
                (byte)(color.A * 255));
        }
    }
}
