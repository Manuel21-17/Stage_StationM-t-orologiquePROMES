using System;
using Xamarin.Forms;
using Xamarin.Forms.Xaml;

namespace STageAPP_1
{
    public partial class App : Application
    {
        public App()
        {
            InitializeComponent();

            /*
             * La barra de título se define aquí y no solo en los
             * recursos de Android para que iOS herede el mismo tema
             * en vez de quedarse con su gris por defecto.
             */
            MainPage = new NavigationPage(new MainPage())
            {
                BarBackgroundColor = Tema.Color("ColorTarjeta"),
                BarTextColor = Tema.Color("ColorTextoPrincipal")
            };
        }

        protected override void OnStart()
        {
        }

        protected override void OnSleep()
        {
        }

        protected override void OnResume()
        {
        }
    }
}
