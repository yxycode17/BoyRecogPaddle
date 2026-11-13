#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <array>
#include <stdexcept>
#include <string>
inline void VerifyModelFile(const std::filesystem::path& path, const char* expected) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Missing model file: " + path.filename().u8string());
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    auto check = [](NTSTATUS s) { if (s < 0) throw std::runtime_error("SHA256 failed"); };
    try {
        check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        check(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
        std::array<unsigned char, 65536> bytes{};
        while (input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || input.gcount())
            check(BCryptHashData(hash, bytes.data(), static_cast<ULONG>(input.gcount()), 0));
        if (!input.eof()) throw std::runtime_error("Model read failed");
        std::array<unsigned char, 32> digest{};
        check(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
        std::string actual;
        for (auto b : digest) { actual += "0123456789abcdef"[b >> 4]; actual += "0123456789abcdef"[b & 15]; }
        if (actual != expected) throw std::runtime_error("Model SHA256 mismatch: " + path.filename().u8string());
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
}
