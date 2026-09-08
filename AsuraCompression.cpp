#include "AsuraCompression.h"

namespace asura {
namespace {

constexpr uint8_t kCompressedMagic[8] = {'A', 's', 'u', 'r', 'a', 'C', 'm', 'p'};
constexpr uint8_t kPlainMagic[8] = {'A', 's', 'u', 'r', 'a', ' ', ' ', ' '};
constexpr uint32_t kCodeLengthCount = 11;
constexpr uint32_t kSymbolCount = 256;
constexpr uint32_t kDecodeTableSize = 1u << kCodeLengthCount;
constexpr uint32_t kPackageSize =
    2 * sizeof(uint32_t) + kCodeLengthCount * sizeof(uint32_t) + kSymbolCount;
constexpr uint32_t kCompressedHeaderSize = sizeof(kCompressedMagic) + kPackageSize;

uint32_t load_u32(const uint8_t* data) {
    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    return value;
}

struct HuffmanPackage {
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t code_length_counts[kCodeLengthCount];
    uint8_t symbol_assignments[kSymbolCount];
};

struct DecodeEntry {
    uint8_t width;
    uint8_t symbol;
    uint8_t padding[2];
};
static_assert(sizeof(DecodeEntry) == sizeof(uint32_t));

bool read_package(const MappedFile& file, HuffmanPackage* package, Error* err) {
    if (file.size < kCompressedHeaderSize)
        return fail(err, "'%s' has a truncated AsuraCmp header", file.path);

    const uint8_t* data = file.data + sizeof(kCompressedMagic);
    package->compressed_size = load_u32(data);
    package->uncompressed_size = load_u32(data + 4);
    for (uint32_t i = 0; i < kCodeLengthCount; ++i)
        package->code_length_counts[i] = load_u32(data + 8 + i * sizeof(uint32_t));
    memcpy(package->symbol_assignments,
           data + 8 + kCodeLengthCount * sizeof(uint32_t), kSymbolCount);

    const uint64_t available = file.size - kCompressedHeaderSize;
    if (package->compressed_size != available)
        return fail(err, "'%s' has an invalid AsuraCmp compressed size (%u, expected %llu)",
                    file.path, package->compressed_size,
                    static_cast<unsigned long long>(available));
    if (!package->compressed_size || (package->compressed_size & 3u))
        return fail(err, "'%s' has a non-word-aligned AsuraCmp stream", file.path);
    if (!package->uncompressed_size)
        return fail(err, "'%s' has an empty AsuraCmp output", file.path);
    if (sizeof(SIZE_T) < sizeof(uint64_t) &&
        static_cast<uint64_t>(package->uncompressed_size) > static_cast<uint64_t>(SIZE_MAX))
        return fail(err, "'%s' is too large to decompress in this editor build", file.path);

    // A complete 11-bit lookup table has a Kraft sum of 2^11. There can be
    // 257 leaves: the package stores 256 byte assignments and the original
    // decoder uses 0xff for the final synthetic entry.
    uint64_t kraft_sum = 0;
    uint64_t leaf_count = 0;
    for (uint32_t depth = 1; depth <= kCodeLengthCount; ++depth) {
        const uint32_t count = package->code_length_counts[depth - 1];
        leaf_count += count;
        kraft_sum += static_cast<uint64_t>(count) << (kCodeLengthCount - depth);
    }
    if (kraft_sum != kDecodeTableSize || leaf_count > kSymbolCount + 1)
        return fail(err, "'%s' has an invalid AsuraCmp Huffman dictionary", file.path);
    return true;
}

bool make_decode_table(const HuffmanPackage& package,
                       DecodeEntry (&table)[kDecodeTableSize], Error* err) {
    memset(table, 0, sizeof(table));
    uint8_t written[kDecodeTableSize]{};
    uint32_t code_offset = 0;
    uint32_t symbols_left = 0;
    for (uint32_t count : package.code_length_counts)
        symbols_left += count;
    int32_t assignment = kSymbolCount - 1;
    uint32_t remaining_entries = kDecodeTableSize;
    uint32_t stride = 1;
    uint32_t carry_scan = 4;

    for (uint32_t depth = 1; depth <= kCodeLengthCount; ++depth) {
        remaining_entries >>= 1;
        stride <<= 1;
        carry_scan <<= 1;
        const uint32_t count = package.code_length_counts[depth - 1];
        for (uint32_t item = 0; item < count; ++item) {
            const uint8_t symbol = assignment >= 0
                                       ? package.symbol_assignments[assignment--]
                                       : 0xff;
            uint32_t table_index = code_offset >> 2;
            for (uint32_t fill = 0; fill < remaining_entries; ++fill) {
                if (table_index >= kDecodeTableSize || written[table_index])
                    return fail(err, "invalid AsuraCmp decode table");
                table[table_index] = {static_cast<uint8_t>(depth), symbol, {0, 0}};
                written[table_index] = 1;
                table_index += stride;
            }

            if (--symbols_left == 0)
                break;
            uint32_t carry = carry_scan;
            do {
                carry >>= 1;
                if (carry & 3u)
                    return fail(err, "incomplete AsuraCmp decode table");
                code_offset ^= carry;
            } while (!(code_offset & carry));
        }
    }
    for (uint8_t entry_written : written) {
        if (!entry_written)
            return fail(err, "incomplete AsuraCmp decode table");
    }
    return true;
}

class HuffmanDecoder {
public:
    HuffmanDecoder(const HuffmanPackage& package, const uint8_t* source,
                   const DecodeEntry* table)
        : package_(package), source_(source), table_(table) {}

    bool decompress(uint8_t* destination, uint32_t destination_size,
                    uint32_t* produced, Error* err) {
        uint8_t* output = destination;
        while (static_cast<uint32_t>(output - destination) < destination_size) {
            available_ += width_;
            fill_ = 31 - available_;
            if (fill_ < 0 || fill_ > 31)
                return fail(err, "invalid AsuraCmp bit state");
            bits_ <<= fill_;
            if (fill_ > reserve_) {
                fill_ -= reserve_;
                available_ += reserve_;
                if (reserve_)
                    bits_ = (reserve_bits_ << (32 - reserve_)) | (bits_ >> reserve_);
                if (bytes_read_ >= package_.compressed_size) {
                    if (bytes_read_ != package_.compressed_size)
                        return fail(err, "AsuraCmp stream overrun");
                    reserve_bits_ = 0;
                } else {
                    if (package_.compressed_size - bytes_read_ < sizeof(uint32_t))
                        return fail(err, "truncated AsuraCmp word");
                    reserve_bits_ = load_u32(source_ + bytes_read_);
                }
                bytes_read_ += sizeof(uint32_t);
                reserve_ = 32;
            }
            if (fill_ <= 0 || fill_ > 31 || fill_ > reserve_)
                return fail(err, "invalid AsuraCmp refill state");
            bits_ = (reserve_bits_ << (32 - fill_)) | (bits_ >> fill_);
            reserve_bits_ >>= fill_;
            reserve_ -= fill_;
            available_ = 31;

            while (available_ >= 0) {
                const DecodeEntry& entry = table_[(bits_ & 0xffeu) >> 1];
                width_ = entry.width;
                if (!width_ || width_ > kCodeLengthCount)
                    return fail(err, "invalid AsuraCmp code width");
                available_ -= width_;
                if (available_ < 0)
                    break;
                bits_ >>= width_;
                *output++ = entry.symbol;
                if (static_cast<uint32_t>(output - destination) == destination_size) {
                    *produced = destination_size;
                    return true;
                }
            }
            // Both the 2005 target and the later reference continue while the
            // signed availability is greater than -32 (0xffffffe0).
            if (available_ <= -32)
                break;
        }
        *produced = static_cast<uint32_t>(output - destination);
        return true;
    }

private:
    const HuffmanPackage& package_;
    const uint8_t* source_;
    const DecodeEntry* table_;
    int32_t available_ = 0;
    int32_t reserve_ = 0;
    int32_t width_ = 0;
    int32_t fill_ = 0;
    uint32_t bits_ = 0;
    uint32_t reserve_bits_ = 0;
    uint32_t bytes_read_ = 0;
};

} // namespace

bool map_asura_file(const char* path, MappedFile* out, Error* err) {
    memset(out, 0, sizeof(*out));
    out->file = INVALID_HANDLE_VALUE;
    MappedFile source{};
    if (!map_file(path, &source, err))
        return false;
    if (source.size < sizeof(kCompressedMagic) ||
        memcmp(source.data, kCompressedMagic, sizeof(kCompressedMagic)) != 0) {
        *out = source;
        return true;
    }

    HuffmanPackage package{};
    DecodeEntry table[kDecodeTableSize]{};
    if (!read_package(source, &package, err) || !make_decode_table(package, table, err)) {
        unmap_file(&source);
        return false;
    }

    uint8_t* expanded = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, static_cast<SIZE_T>(package.uncompressed_size), MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    if (!expanded) {
        const DWORD code = GetLastError();
        unmap_file(&source);
        return fail(err, "cannot allocate %u bytes to decompress '%s' (win32=%lu)",
                    package.uncompressed_size, path, code);
    }

    HuffmanDecoder decoder(package, source.data + kCompressedHeaderSize, table);
    uint32_t produced = 0;
    const bool decoded = decoder.decompress(expanded, package.uncompressed_size, &produced, err);
    if (!decoded || produced != package.uncompressed_size ||
        package.uncompressed_size < sizeof(kPlainMagic) ||
        memcmp(expanded, kPlainMagic, sizeof(kPlainMagic)) != 0) {
        if (decoded && produced != package.uncompressed_size)
            fail(err, "'%s' decompressed to %u bytes, expected %u", path, produced,
                 package.uncompressed_size);
        else if (decoded && !err->set)
            fail(err, "'%s' did not decompress to an Asura file", path);
        VirtualFree(expanded, 0, MEM_RELEASE);
        unmap_file(&source);
        return false;
    }

    unmap_file(&source);
    out->file = INVALID_HANDLE_VALUE;
    out->data = expanded;
    out->owned_data = expanded;
    out->size = package.uncompressed_size;
    out->path = path;
    return true;
}

} // namespace asura
