#pragma once
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>
#include <cstdint>

namespace wmf::update {
using Progress = std::function<void(std::uint64_t,std::uint64_t)>;
struct File { std::filesystem::path path; std::uint64_t bytes{}; std::string sha256; };
struct Manifest { std::string version; std::vector<File> files; };
std::filesystem::path SafeRelative(std::string_view);
void CheckTree(const std::filesystem::path&);
std::string Sha256(const std::filesystem::path&,std::stop_token = {});
std::string ReadText(const std::filesystem::path&,std::size_t limit = 4*1024*1024);
void WriteText(const std::filesystem::path&,std::string_view);
Manifest ReadManifest(const std::filesystem::path&);
Manifest VerifyPackage(const std::filesystem::path&,std::string_view expectedVersion,std::stop_token = {},Progress = {});
void ExtractPackage(const std::filesystem::path& zip,const std::filesystem::path& destination,std::stop_token,Progress = {});
// The journal and backups are durable before the first installed file is changed.
void PrepareTransaction(const std::filesystem::path& transaction,const std::filesystem::path& data);
void ApplyTransaction(const std::filesystem::path& transaction,Progress = {});
void RollbackTransaction(const std::filesystem::path& transaction,Progress = {});
void CommitTransaction(const std::filesystem::path& transaction);
bool TransactionPending(const std::filesystem::path& root);
std::filesystem::path PendingTransaction(const std::filesystem::path& root);
void CheckTransactionPath(const std::filesystem::path&);
void CleanupTransactions(const std::filesystem::path&) noexcept;
}
