// Compiled with link-time optimisation, which the export list must refuse.
namespace ExportListFixture
{
int linkTimeCodeFunction(int value)
{
    return value + 1;
}
} // namespace ExportListFixture
