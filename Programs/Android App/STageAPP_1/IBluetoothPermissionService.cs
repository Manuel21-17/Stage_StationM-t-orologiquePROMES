using System.Threading.Tasks;

namespace STageAPP_1
{
    public interface IBluetoothPermissionService
    {
        Task<bool> SolicitarPermisosAsync();
    }
}
