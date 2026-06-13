#include "zip_archive_internal.h"

#include <random>

namespace cozip::format_zip
{
namespace
{
class ZipTraditionalCipher
{
public:
    explicit ZipTraditionalCipher(std::string_view password)
    {
        Reset(password);
    }

    void Reset(std::string_view password)
    {
        keys_[0] = 305419896u;
        keys_[1] = 591751049u;
        keys_[2] = 878082192u;

        for (const char ch : password)
        {
            UpdateKeys(static_cast<std::uint8_t>(ch));
        }
    }

    [[nodiscard]] std::byte Encrypt(std::byte plain_byte)
    {
        const auto plain = std::to_integer<std::uint8_t>(plain_byte);
        const auto cipher = static_cast<std::uint8_t>(plain ^ DecryptByte());
        UpdateKeys(plain);
        return static_cast<std::byte>(cipher);
    }

    [[nodiscard]] std::byte Decrypt(std::byte cipher_byte)
    {
        const auto cipher = std::to_integer<std::uint8_t>(cipher_byte);
        const auto plain = static_cast<std::uint8_t>(cipher ^ DecryptByte());
        UpdateKeys(plain);
        return static_cast<std::byte>(plain);
    }

private:
    void UpdateKeys(std::uint8_t value)
    {
        keys_[0] = static_cast<std::uint32_t>(
            mz_crc32(keys_[0], &value, 1));
        keys_[1] = keys_[1] + (keys_[0] & 0xffu);
        keys_[1] = keys_[1] * 134775813u + 1u;
        const auto high_byte = static_cast<std::uint8_t>((keys_[1] >> 24) & 0xffu);
        keys_[2] = static_cast<std::uint32_t>(
            mz_crc32(keys_[2], &high_byte, 1));
    }

    [[nodiscard]] std::uint8_t DecryptByte() const noexcept
    {
        const auto temp = static_cast<std::uint16_t>((keys_[2] & 0xffffu) | 2u);
        return static_cast<std::uint8_t>(((temp * (temp ^ 1u)) >> 8) & 0xffu);
    }

    std::array<std::uint32_t, 3> keys_ {};
};

std::uint16_t PasswordVerifier(const ZipEntrySource& entry) noexcept
{
    return static_cast<std::uint16_t>((entry.crc32 >> 16) & 0xffffu);
}
}

core::EncryptionMode ResolveZipEncryptionMode(const core::ExecutionOptions& execution) noexcept
{
    if (execution.encryption.mode != core::EncryptionMode::None)
    {
        return execution.encryption.mode;
    }

    if (!execution.encryption.password.empty())
    {
        return core::EncryptionMode::ZipTraditional;
    }

    return core::EncryptionMode::None;
}

bool IsZipEntryEncrypted(const ZipCentralDirectoryEntry& entry) noexcept
{
    return (entry.general_purpose_flag & kEncryptedFlag) != 0u;
}

ZipOperationResult ValidateZipEncryptionOptions(const core::ExecutionOptions& execution)
{
    const auto mode = ResolveZipEncryptionMode(execution);
    if (mode == core::EncryptionMode::None)
    {
        return {ZipStatus::Ok, {}};
    }

    if (execution.encryption.password.empty())
    {
        return MakeError(ZipStatus::InvalidJob, "zip password is required when encryption is enabled");
    }

    if (mode != core::EncryptionMode::ZipTraditional)
    {
        return MakeError(ZipStatus::Unsupported, "requested zip encryption mode is not implemented");
    }

    return {ZipStatus::Ok, {}};
}

ZipOperationResult EncryptZipEntryPayload(std::span<const std::byte> compressed_bytes,
                                          const ZipEntrySource& entry,
                                          const core::ExecutionOptions& execution,
                                          std::vector<std::byte>& encrypted_bytes)
{
    const auto mode = ResolveZipEncryptionMode(execution);
    if (mode == core::EncryptionMode::None)
    {
        encrypted_bytes.assign(compressed_bytes.begin(), compressed_bytes.end());
        return {ZipStatus::Ok, {}};
    }

    auto validation = ValidateZipEncryptionOptions(execution);
    if (validation.status != ZipStatus::Ok)
    {
        return validation;
    }

    ZipTraditionalCipher cipher(execution.encryption.password);
    encrypted_bytes.resize(kZipTraditionalEncryptionHeaderSize + compressed_bytes.size());

    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<unsigned int> distribution(0, 255);

    std::array<std::byte, kZipTraditionalEncryptionHeaderSize> header {};
    for (std::size_t index = 0; index + 2 < header.size(); ++index)
    {
        header[index] = static_cast<std::byte>(distribution(generator));
    }

    const auto verifier = PasswordVerifier(entry);
    header[header.size() - 2] = static_cast<std::byte>(verifier & 0xffu);
    header[header.size() - 1] = static_cast<std::byte>((verifier >> 8) & 0xffu);

    for (std::size_t index = 0; index < header.size(); ++index)
    {
        encrypted_bytes[index] = cipher.Encrypt(header[index]);
    }

    for (std::size_t index = 0; index < compressed_bytes.size(); ++index)
    {
        encrypted_bytes[kZipTraditionalEncryptionHeaderSize + index] = cipher.Encrypt(compressed_bytes[index]);
    }

    return {ZipStatus::Ok, {}};
}

ZipOperationResult DecryptZipEntryPayload(std::span<const std::byte> encrypted_bytes,
                                          const ZipCentralDirectoryEntry& entry,
                                          std::uint16_t password_verifier,
                                          const core::ExecutionOptions& execution,
                                          std::vector<std::byte>& decrypted_bytes)
{
    if (!IsZipEntryEncrypted(entry))
    {
        decrypted_bytes.assign(encrypted_bytes.begin(), encrypted_bytes.end());
        return {ZipStatus::Ok, {}};
    }

    if (entry.compression_method == 99u)
    {
        return MakeError(ZipStatus::Unsupported, "winzip aes encrypted entries are not implemented: " + entry.name);
    }

    auto validation = ValidateZipEncryptionOptions(execution);
    if (validation.status != ZipStatus::Ok)
    {
        return validation;
    }

    if (encrypted_bytes.size() < kZipTraditionalEncryptionHeaderSize)
    {
        return MakeError(ZipStatus::InvalidJob, "encrypted zip entry is truncated: " + entry.name);
    }

    ZipTraditionalCipher cipher(execution.encryption.password);
    std::array<std::byte, kZipTraditionalEncryptionHeaderSize> header {};
    for (std::size_t index = 0; index < header.size(); ++index)
    {
        header[index] = cipher.Decrypt(encrypted_bytes[index]);
    }

    const auto expected_low = static_cast<std::uint8_t>(password_verifier & 0xffu);
    const auto expected_high = static_cast<std::uint8_t>((password_verifier >> 8) & 0xffu);
    if (std::to_integer<std::uint8_t>(header[header.size() - 2]) != expected_low ||
        std::to_integer<std::uint8_t>(header[header.size() - 1]) != expected_high)
    {
        return MakeError(ZipStatus::InvalidJob, "zip password is incorrect: " + entry.name);
    }

    decrypted_bytes.resize(encrypted_bytes.size() - kZipTraditionalEncryptionHeaderSize);
    for (std::size_t index = 0; index < decrypted_bytes.size(); ++index)
    {
        decrypted_bytes[index] = cipher.Decrypt(encrypted_bytes[kZipTraditionalEncryptionHeaderSize + index]);
    }

    return {ZipStatus::Ok, {}};
}
}
