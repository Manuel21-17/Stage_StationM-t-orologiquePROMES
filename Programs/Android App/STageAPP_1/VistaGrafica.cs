using System.Collections.Generic;
using SkiaSharp;
using SkiaSharp.Views.Forms;

namespace STageAPP_1
{
    /*
     * Lienzo de una gráfica. Sustituye al ChartView de Microcharts.
     *
     * Microcharts solo sabe pintar la serie y las etiquetas de cada
     * punto: no tiene ejes, ni rejilla, ni escala numérica, y no hay
     * forma de configurárselos. Este control delega el dibujo en
     * PintorGrafica, que sí los hace.
     *
     * Aquí no hay lógica de dibujo a propósito: toda vive en
     * PintorGrafica, que no depende de Xamarin y por eso se puede
     * ejecutar fuera de la app para generar vistas previas.
     */
    public class VistaGrafica : SKCanvasView
    {
        private readonly List<float> valores = new List<float>();

        public OpcionesGrafica Opciones { get; set; }
            = new OpcionesGrafica();

        public PaletaGrafica Paleta { get; set; }
            = new PaletaGrafica();

        /*
         * Sustituye la serie completa y repinta. Se copia en lugar de
         * guardar la referencia: quien llama suele reutilizar su propia
         * lista, y compartirla haría que la gráfica cambiara sola a
         * mitad de un dibujo.
         */
        public void Mostrar(IList<float> nuevosValores)
        {
            valores.Clear();

            if (nuevosValores != null)
            {
                valores.AddRange(nuevosValores);
            }

            InvalidateSurface();
        }

        protected override void OnPaintSurface(
            SKPaintSurfaceEventArgs e)
        {
            base.OnPaintSurface(e);

            SKCanvas lienzo = e.Surface.Canvas;

            lienzo.Clear();

            PintorGrafica.Dibujar(
                lienzo,
                new SKRect(0, 0, e.Info.Width, e.Info.Height),
                valores,
                Opciones,
                Paleta);
        }
    }
}
