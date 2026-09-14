using System;
using System.Collections.Generic;
using System.Globalization;
using SkiaSharp;

namespace STageAPP_1
{
    /*
     * Dibujante de gráficas con ejes, rejilla y escala.
     *
     * Existe porque Microcharts no sabe hacer esto: solo pinta la serie
     * y las etiquetas de cada punto, sin ejes, sin rejilla y sin escala
     * numérica. Para el diseño que se pidió no había forma de
     * conseguirlo configurándolo.
     *
     * Es código de SkiaSharp puro, SIN dependencias de Xamarin.Forms, a
     * propósito: así el mismo dibujante que usa la aplicación se puede
     * ejecutar en un programa de escritorio para generar una vista
     * previa en PNG. Lo que se ve en esa imagen es literalmente lo que
     * pinta la app.
     */
    public sealed class OpcionesGrafica
    {
        // Escala vertical fija. No se autoajusta con los datos: una
        // escala que baila con cada muestra hace imposible comparar de
        // un vistazo.
        public float MinimoY = 0;
        public float MaximoY = 100;

        // Número de franjas horizontales de la rejilla.
        public int DivisionesY = 5;

        // Número de franjas verticales.
        public int DivisionesX = 6;

        public string FormatoY = "0.#";

        // Ancho de la ventana temporal, en muestras.
        public int MuestrasVisibles = 30;

        public SKColor ColorSerie = SKColors.Orange;

        // La lluvia se dibuja en barras, como en el panel de Grafana.
        public bool Barras = false;

        // Texto que se muestra cuando todavía no hay ninguna muestra.
        public string TextoSinDatos = "Sin datos";
    }

    public sealed class PaletaGrafica
    {
        public SKColor Fondo = SKColor.Parse("#1E2128");
        public SKColor Rejilla = SKColor.Parse("#2E3440");
        public SKColor Ejes = SKColor.Parse("#4A5262");
        public SKColor Texto = SKColor.Parse("#9AA3B2");
    }

    public static class PintorGrafica
    {
        public static void Dibujar(
            SKCanvas lienzo,
            SKRect zona,
            IList<float> valores,
            OpcionesGrafica opciones,
            PaletaGrafica paleta)
        {
            if (opciones == null)
                opciones = new OpcionesGrafica();

            if (paleta == null)
                paleta = new PaletaGrafica();

            /*
             * El tamaño del texto se calcula a partir de la altura, no
             * fijo: el lienzo llega en píxeles físicos y en un teléfono
             * de densidad alta un valor fijo saldría diminuto.
             */
            float tamanoTexto = Limitar(zona.Height * 0.075f, 11f, 34f);

            using (SKPaint tinta = new SKPaint { IsAntialias = true })
            {
                tinta.TextSize = tamanoTexto;
                tinta.Typeface = SKTypeface.Default;

                // Espacio para las etiquetas del eje vertical.
                float anchoEtiquetaY = MedirAnchoEtiquetasY(
                    tinta, opciones);

                float margenIzq = anchoEtiquetaY + tamanoTexto * 0.6f;
                float margenAbajo = tamanoTexto * 1.9f;
                float margenArriba = tamanoTexto * 0.8f;
                float margenDer = tamanoTexto * 0.8f;

                SKRect area = new SKRect(
                    zona.Left + margenIzq,
                    zona.Top + margenArriba,
                    zona.Right - margenDer,
                    zona.Bottom - margenAbajo);

                if (area.Width <= 1 || area.Height <= 1)
                    return;

                DibujarRejilla(lienzo, area, tinta, opciones, paleta);

                DibujarEtiquetasY(lienzo, area, tinta, opciones, paleta);

                DibujarEtiquetasX(lienzo, area, tinta, opciones, paleta);

                DibujarMarco(lienzo, area, tinta, paleta);

                if (valores == null || valores.Count == 0)
                {
                    DibujarSinDatos(
                        lienzo, area, tinta, opciones, paleta);

                    return;
                }

                if (opciones.Barras)
                {
                    DibujarBarras(
                        lienzo, area, tinta, valores, opciones);
                }
                else
                {
                    DibujarLinea(
                        lienzo, area, tinta, valores, opciones);
                }
            }
        }

        // ============================================================

        private static void DibujarRejilla(
            SKCanvas lienzo,
            SKRect area,
            SKPaint tinta,
            OpcionesGrafica opciones,
            PaletaGrafica paleta)
        {
            tinta.Style = SKPaintStyle.Stroke;
            tinta.Color = paleta.Rejilla;
            tinta.StrokeWidth = 1f;

            for (int i = 1; i < opciones.DivisionesY; i++)
            {
                float y = area.Bottom -
                    (area.Height * i / opciones.DivisionesY);

                lienzo.DrawLine(area.Left, y, area.Right, y, tinta);
            }

            for (int i = 1; i < opciones.DivisionesX; i++)
            {
                float x = area.Left +
                    (area.Width * i / opciones.DivisionesX);

                lienzo.DrawLine(x, area.Top, x, area.Bottom, tinta);
            }
        }

        private static void DibujarMarco(
            SKCanvas lienzo,
            SKRect area,
            SKPaint tinta,
            PaletaGrafica paleta)
        {
            tinta.Style = SKPaintStyle.Stroke;
            tinta.Color = paleta.Ejes;
            tinta.StrokeWidth = 1.4f;

            lienzo.DrawRect(area, tinta);
        }

        private static void DibujarEtiquetasY(
            SKCanvas lienzo,
            SKRect area,
            SKPaint tinta,
            OpcionesGrafica opciones,
            PaletaGrafica paleta)
        {
            tinta.Style = SKPaintStyle.Fill;
            tinta.Color = paleta.Texto;
            tinta.TextAlign = SKTextAlign.Right;

            for (int i = 0; i <= opciones.DivisionesY; i++)
            {
                float fraccion = i / (float)opciones.DivisionesY;

                float valor = opciones.MinimoY +
                    (opciones.MaximoY - opciones.MinimoY) * fraccion;

                float y = area.Bottom - area.Height * fraccion;

                string texto = valor.ToString(
                    opciones.FormatoY, CultureInfo.InvariantCulture);

                lienzo.DrawText(
                    texto,
                    area.Left - tinta.TextSize * 0.4f,
                    y + tinta.TextSize * 0.35f,
                    tinta);
            }
        }

        /*
         * El eje horizontal es la ventana de muestras, con la más
         * reciente a la derecha. Se rotula en negativo (-30, -24, ...,
         * 0) porque lo que interesa es "hace cuántas muestras", no un
         * índice absoluto que crecería sin fin.
         */
        private static void DibujarEtiquetasX(
            SKCanvas lienzo,
            SKRect area,
            SKPaint tinta,
            OpcionesGrafica opciones,
            PaletaGrafica paleta)
        {
            tinta.Style = SKPaintStyle.Fill;
            tinta.Color = paleta.Texto;
            tinta.TextAlign = SKTextAlign.Center;

            for (int i = 0; i <= opciones.DivisionesX; i++)
            {
                float fraccion = i / (float)opciones.DivisionesX;

                int muestra = (int)Math.Round(
                    -opciones.MuestrasVisibles * (1f - fraccion));

                float x = area.Left + area.Width * fraccion;

                lienzo.DrawText(
                    muestra.ToString(CultureInfo.InvariantCulture),
                    x,
                    area.Bottom + tinta.TextSize * 1.3f,
                    tinta);
            }
        }

        private static void DibujarLinea(
            SKCanvas lienzo,
            SKRect area,
            SKPaint tinta,
            IList<float> valores,
            OpcionesGrafica opciones)
        {
            int n = valores.Count;

            SKPoint[] puntos = new SKPoint[n];

            for (int i = 0; i < n; i++)
            {
                puntos[i] = new SKPoint(
                    CoordenadaX(area, i, n, opciones),
                    CoordenadaY(area, valores[i], opciones));
            }

            tinta.Style = SKPaintStyle.Stroke;
            tinta.Color = opciones.ColorSerie;
            tinta.StrokeWidth = Math.Max(1.6f, area.Height * 0.012f);
            tinta.StrokeCap = SKStrokeCap.Round;
            tinta.StrokeJoin = SKStrokeJoin.Round;

            for (int i = 1; i < n; i++)
            {
                lienzo.DrawLine(
                    puntos[i - 1], puntos[i], tinta);
            }

            float radio = Math.Max(2.2f, area.Height * 0.018f);

            tinta.Style = SKPaintStyle.Fill;

            for (int i = 0; i < n; i++)
            {
                lienzo.DrawCircle(puntos[i], radio, tinta);
            }
        }

        private static void DibujarBarras(
            SKCanvas lienzo,
            SKRect area,
            SKPaint tinta,
            IList<float> valores,
            OpcionesGrafica opciones)
        {
            int n = valores.Count;

            float paso = area.Width /
                Math.Max(1, opciones.MuestrasVisibles);

            float ancho = Math.Max(2f, paso * 0.65f);

            float baseY = CoordenadaY(area, 0, opciones);

            tinta.Style = SKPaintStyle.Fill;
            tinta.Color = opciones.ColorSerie;

            for (int i = 0; i < n; i++)
            {
                float x = CoordenadaX(area, i, n, opciones);
                float y = CoordenadaY(area, valores[i], opciones);

                float arriba = Math.Min(y, baseY);
                float abajo = Math.Max(y, baseY);

                // Una barra de altura cero no se vería; se le da un
                // mínimo para que el cero siga siendo legible.
                if (abajo - arriba < 1.2f)
                {
                    abajo = arriba + 1.2f;
                }

                lienzo.DrawRect(
                    new SKRect(x - ancho / 2f, arriba,
                               x + ancho / 2f, abajo),
                    tinta);
            }
        }

        private static void DibujarSinDatos(
            SKCanvas lienzo,
            SKRect area,
            SKPaint tinta,
            OpcionesGrafica opciones,
            PaletaGrafica paleta)
        {
            tinta.Style = SKPaintStyle.Fill;
            tinta.Color = paleta.Texto;
            tinta.TextAlign = SKTextAlign.Center;

            lienzo.DrawText(
                opciones.TextoSinDatos,
                area.MidX,
                area.MidY + tinta.TextSize * 0.35f,
                tinta);
        }

        // ============================================================

        /*
         * La muestra más reciente se ancla al borde derecho y las
         * anteriores se reparten hacia la izquierda. Así la curva
         * avanza siempre contra el borde en vez de estirarse y
         * encogerse según cuántas muestras haya.
         */
        private static float CoordenadaX(
            SKRect area,
            int indice,
            int total,
            OpcionesGrafica opciones)
        {
            int ventana = Math.Max(1, opciones.MuestrasVisibles - 1);

            int desdeElFinal = (total - 1) - indice;

            float fraccion = 1f - (desdeElFinal / (float)ventana);

            return area.Left + area.Width * Limitar(fraccion, 0f, 1f);
        }

        private static float CoordenadaY(
            SKRect area,
            float valor,
            OpcionesGrafica opciones)
        {
            float rango = opciones.MaximoY - opciones.MinimoY;

            if (Math.Abs(rango) < float.Epsilon)
                return area.Bottom;

            float fraccion = (valor - opciones.MinimoY) / rango;

            return area.Bottom -
                area.Height * Limitar(fraccion, 0f, 1f);
        }

        private static float MedirAnchoEtiquetasY(
            SKPaint tinta,
            OpcionesGrafica opciones)
        {
            float ancho = 0;

            for (int i = 0; i <= opciones.DivisionesY; i++)
            {
                float fraccion = i / (float)opciones.DivisionesY;

                float valor = opciones.MinimoY +
                    (opciones.MaximoY - opciones.MinimoY) * fraccion;

                string texto = valor.ToString(
                    opciones.FormatoY, CultureInfo.InvariantCulture);

                float w = tinta.MeasureText(texto);

                if (w > ancho)
                    ancho = w;
            }

            return ancho;
        }

        private static float Limitar(float v, float min, float max)
        {
            if (v < min) return min;
            if (v > max) return max;
            return v;
        }
    }
}
