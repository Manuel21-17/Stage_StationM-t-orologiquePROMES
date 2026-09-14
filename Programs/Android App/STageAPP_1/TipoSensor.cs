namespace STageAPP_1
{
    /*
     * Tipo de sensor conectado a una entrada.
     *
     * Los valores son explícitos porque se guardan en disco: cambiar
     * un número rompería la configuración ya almacenada en los
     * teléfonos que tengan la app instalada.
     */
    public enum TipoSensor
    {
        SinAsignar = 0,
        Meteorologico = 1,
        Corriente = 2,
        Voltaje = 3
    }
}
