using System;
using Xamarin.Forms;

namespace STageAPP_1
{
    public partial class VisualizarDatosPage : ContentPage
    {
        public VisualizarDatosPage()
        {
            InitializeComponent();
        }

        private async void OnDatosMeteorologicosClicked(
            object sender,
            EventArgs e)
        {
            await Navigation.PushAsync(new DatosMeteorologicosPage());
        }

        private async void OnDatosElectricosClicked(
            object sender,
            EventArgs e)
        {
            await Navigation.PushAsync(new DatosElectricosPage());
        }
    }
}