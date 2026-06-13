namespace Cozip.Gui.Models
{
    public sealed class ArchiveEntryRow
    {
        public string Kind { get; set; } = string.Empty;
        public string Name { get; set; } = string.Empty;
        public string Method { get; set; } = string.Empty;
        public ulong Size { get; set; }
        public ulong PackedSize { get; set; }
        public string SizeDisplay { get; set; } = string.Empty;
        public string PackedDisplay { get; set; } = string.Empty;
        public string Crc32 { get; set; } = string.Empty;
    }
}
