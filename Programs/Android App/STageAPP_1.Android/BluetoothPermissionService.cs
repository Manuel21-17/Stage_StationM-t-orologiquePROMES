using System.Threading.Tasks;
using Android;
using Android.Content.PM;
using Android.OS;
using AndroidX.Core.App;
using AndroidX.Core.Content;
using Xamarin.Essentials;
using Xamarin.Forms;
using STageAPP_1.Droid;

[assembly: Dependency(typeof(BluetoothPermissionService))]

namespace STageAPP_1.Droid
{
    public class BluetoothPermissionService :
        IBluetoothPermissionService
    {
        private const int CodigoSolicitudBluetooth = 1200;

        public async Task<bool> SolicitarPermisosAsync()
        {
            var actividad = Platform.CurrentActivity;

            if (actividad == null)
                return false;

            // Android 12 o superior
            if (Build.VERSION.SdkInt >= BuildVersionCodes.S)
            {
                bool permisoEscaneo =
                    ContextCompat.CheckSelfPermission(
                        actividad,
                        Manifest.Permission.BluetoothScan)
                    == Permission.Granted;

                bool permisoConexion =
                    ContextCompat.CheckSelfPermission(
                        actividad,
                        Manifest.Permission.BluetoothConnect)
                    == Permission.Granted;

                if (permisoEscaneo && permisoConexion)
                    return true;

                ActivityCompat.RequestPermissions(
                    actividad,
                    new[]
                    {
                        Manifest.Permission.BluetoothScan,
                        Manifest.Permission.BluetoothConnect
                    },
                    CodigoSolicitudBluetooth);

                // Esperar a que el usuario responda al diálogo.
                for (int intento = 0; intento < 40; intento++)
                {
                    await Task.Delay(250);

                    permisoEscaneo =
                        ContextCompat.CheckSelfPermission(
                            actividad,
                            Manifest.Permission.BluetoothScan)
                        == Permission.Granted;

                    permisoConexion =
                        ContextCompat.CheckSelfPermission(
                            actividad,
                            Manifest.Permission.BluetoothConnect)
                        == Permission.Granted;

                    if (permisoEscaneo && permisoConexion)
                        return true;
                }

                return false;
            }

            // Android 11 o anterior
            PermissionStatus estado =
                await Permissions.CheckStatusAsync<
                    Permissions.LocationWhenInUse>();

            if (estado != PermissionStatus.Granted)
            {
                estado =
                    await Permissions.RequestAsync<
                        Permissions.LocationWhenInUse>();
            }

            return estado == PermissionStatus.Granted;
        }
    }
}